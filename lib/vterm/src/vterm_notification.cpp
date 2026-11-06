// The desktop notifications that a program posts: OSC 9, OSC 777 and OSC 99, and the queue from which the caller takes
// them.

#include "vterm/src/vterm_notification.h"

#include "pr/base64.h"
#include "pr/mem.h"
#include "pr/utf8.h"

#include "vterm/src/vterm_screen.h"

namespace pr_impl {

namespace {

// The number of ConEmu's last command of OSC 9. The commands are numbered from 1.
constexpr uint32_t kConEmuLastCommand = 12;

// The part of a notification that a payload of OSC 99 carries.
enum {
    KittyPayload_Title,
    KittyPayload_Body,
    KittyPayload_Query,
    KittyPayload_Other,         // a part that the terminal does not show, such as an icon or the buttons
};
typedef uint8_t KittyPayload;

bool isSameText(PrStringView a, PrStringView b) {
    return a.size == b.size && (a.size == 0 || prMemCompare(a.data, b.data, a.size) == 0);
}

PrStringView viewOf(const RawArray<char>& bytes) {
    return PrStringView{bytes.data, bytes.count};
}

// Splits `*text` at its first `separator`: returns what comes before it, and leaves what follows in `*text`.
PrStringView splitAt(PrStringView* text, char separator) {
    size_t end = 0;
    while (end < text->size && text->data[end] != separator)
        ++end;
    PrStringView field{text->data, end};
    size_t next = end < text->size ? end + 1 : end;
    *text = PrStringView{text->data + next, text->size - next};
    return field;
}

// Returns true if the text of OSC 9 is one of ConEmu's other commands: a command number, alone or before a semicolon.
bool isConEmuCommand(PrStringView text) {
    PrStringView field = splitAt(&text, ';');
    if (field.size == 0 || field.size > 2)
        return false;
    uint32_t value = 0;
    for (size_t i = 0; i < field.size; ++i) {
        if (field.data[i] < '0' || field.data[i] > '9')
            return false;
        value = value * 10 + uint32_t(field.data[i] - '0');
    }
    return value >= 1 && value <= kConEmuLastCommand;
}

// Returns true if `id` is an identifier of OSC 99: letters, digits, and `-`, `_`, `+` and `.`.
bool isKittyId(PrStringView id) {
    for (size_t i = 0; i < id.size; ++i) {
        char c = id.data[i];
        bool alphanumeric = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
        if (!alphanumeric && c != '-' && c != '_' && c != '+' && c != '.')
            return false;
    }
    return true;
}

// Appends the code points of `bytes` to `out`, up to `limit` bytes, and drops control characters. If `lineFeeds` is
// set, line feeds are kept. Returns false if `bytes` is not valid UTF-8 before the limit. Bytes past the limit may be
// invalid, since a draft cuts its bytes there and may split a sequence.
bool appendShown(PrRawString* out, PrStringView bytes, size_t limit, bool lineFeeds) {
    size_t valid = prUtf8Validate(bytes);
    if (valid < bytes.size && valid < limit)
        return false;
    size_t written = 0;
    size_t at = 0;
    while (at < valid) {
        uint32_t codePoint = 0;
        uint32_t size = prUtf8Decode(bytes, at, &codePoint);
        PrStringView encoded{bytes.data + at, size};
        at += size;
        bool control = codePoint < 0x20 || (codePoint >= 0x7f && codePoint < 0xa0);
        if (control && !(lineFeeds && codePoint == '\n'))
            continue;
        if (written + size > limit)
            break;
        prRawStringAppend(out, encoded);
        written += size;
    }
    return true;
}

// Queues a notification of `title` and `body`, which the program wrote. It drops a notification that shows no text or
// whose text is not UTF-8.
void post(Vterm* vterm, PrStringView title, PrStringView body, PrVtermNotificationCondition condition) {
    PrRawString text = prRawStringNull();
    bool valid = appendShown(&text, title, kPrVtermNotificationMaxTitleBytes, false);
    uint32_t titleSize = uint32_t(prRawStringView(&text).size);
    valid = valid && appendShown(&text, body, kPrVtermNotificationMaxBodyBytes, true);
    if (!valid || prRawStringView(&text).size == 0) {
        prRawStringReset(&text);
        return;
    }
    VtermNotifications& notifications = vterm->notifications;
    if (notifications.queue.count == kPrVtermMaxNotifications) {
        prRawStringReset(&notifications.queue.data[0].text);
        notifications.queue.erase(0);
    }
    notifications.queue.push(VtermNotificationRecord{.text = text, .titleSize = titleSize, .condition = condition});
    vterm->events |= PrVtermEventFlag_Notification;
}

void putTerminator(Vterm* vterm, uint8_t terminator) {
    if (terminator == 0x07)
        vtermPutByte(vterm, 0x07);
    else
        vtermPut(vterm, "\x1b\\");
}

// Answers the query of OSC 99 with the parts of the protocol that the terminal supports.
void kittyAnswer(Vterm* vterm, PrStringView id, uint8_t terminator) {
    vtermPut(vterm, "\x1b]99;");
    if (id.size != 0) {
        vtermPut(vterm, "i=");
        vtermPut(vterm, id);
        vtermPutByte(vterm, ':');
    }
    vtermPut(vterm, "p=?;a=focus:o=always,unfocused,invisible:p=title,body,?");
    putTerminator(vterm, terminator);
}

// Adds a chunk's payload to `target`, decoded from base64 if `base64` is set. Keeps at most `limit` bytes plus one code
// point. Returns false if the payload is not valid base64.
bool appendPayload(RawArray<char>* target, PrStringView payload, bool base64, size_t limit) {
    uint32_t start = target->count;
    if (base64) {
        size_t capacity = prBase64DecodedCapacity(payload.size);
        target->resize(start + uint32_t(capacity));
        size_t size = 0;
        if (!prBase64Decode(payload, target->data + start, capacity, &size))
            return false;
        target->resize(start + uint32_t(size));
    } else {
        target->append(payload.data, uint32_t(payload.size));
    }
    size_t kept = limit + kPrUtf8MaxBytes;
    if (target->count > kept)
        target->resize(uint32_t(kept));
    return true;
}

// Runs OSC 99: metadata of keys and values, followed by a payload.
void kittyCommand(Vterm* vterm, PrStringView rest, uint8_t terminator) {
    PrStringView metadata = splitAt(&rest, ';');
    PrStringView payload = rest;
    PrStringView id{};
    bool done = true;
    bool base64 = false;
    KittyPayload kind = KittyPayload_Title;
    bool hasCondition = false;
    PrVtermNotificationCondition condition = PrVtermNotificationCondition_None;
    while (metadata.size != 0) {
        PrStringView value = splitAt(&metadata, ':');
        PrStringView key = splitAt(&value, '=');
        if (key == "i" && isKittyId(value)) {
            id = value;
        } else if (key == "d") {
            done = !(value == "0");
        } else if (key == "e") {
            base64 = value == "1";
        } else if (key == "p") {
            kind = KittyPayload_Other;
            if (value == "title")
                kind = KittyPayload_Title;
            else if (value == "body")
                kind = KittyPayload_Body;
            else if (value == "?")
                kind = KittyPayload_Query;
        } else if (key == "o") {
            hasCondition = true;
            condition = PrVtermNotificationCondition_None;
            if (value == "always")
                condition = PrVtermNotificationCondition_Always;
            else if (value == "unfocused")
                condition = PrVtermNotificationCondition_Unfocused;
            else if (value == "invisible")
                condition = PrVtermNotificationCondition_Invisible;
        }
    }
    if (kind == KittyPayload_Query) {
        kittyAnswer(vterm, id, terminator);
        return;
    }
    VtermNotifications& notifications = vterm->notifications;
    VtermNotificationDraft& draft = notifications.draft;
    if (draft.open && !isSameText(viewOf(draft.id), id))
        vtermNotificationsDropDraft(&notifications);
    if (!draft.open) {
        draft.open = true;
        draft.id.append(id.data, uint32_t(id.size));
        draft.condition = PrVtermNotificationCondition_None;
    }
    if (hasCondition)
        draft.condition = condition;
    bool decoded = true;
    if (kind == KittyPayload_Title)
        decoded = appendPayload(&draft.title, payload, base64, kPrVtermNotificationMaxTitleBytes);
    else if (kind == KittyPayload_Body)
        decoded = appendPayload(&draft.body, payload, base64, kPrVtermNotificationMaxBodyBytes);
    if (!decoded) {
        vtermNotificationsDropDraft(&notifications);
        return;
    }
    if (done) {
        post(vterm, viewOf(draft.title), viewOf(draft.body), draft.condition);
        vtermNotificationsDropDraft(&notifications);
    }
}

void resetTexts(RawArray<VtermNotificationRecord>* notifications) {
    for (VtermNotificationRecord& notification : notifications->span())
        prRawStringReset(&notification.text);
    notifications->clear();
}

} // namespace

void vtermNotificationsReset(VtermNotifications* notifications) {
    vtermNotificationsClear(notifications);
    notifications->queue.reset();
    notifications->taken.reset();
    notifications->draft.id.reset();
    notifications->draft.title.reset();
    notifications->draft.body.reset();
}

void vtermNotificationsClear(VtermNotifications* notifications) {
    resetTexts(&notifications->queue);
    resetTexts(&notifications->taken);
    vtermNotificationsDropDraft(notifications);
}

void vtermNotificationsDropDraft(VtermNotifications* notifications) {
    VtermNotificationDraft& draft = notifications->draft;
    draft.open = false;
    draft.id.clear();
    draft.title.clear();
    draft.body.clear();
}

void vtermNotificationCommand(Vterm* vterm, uint32_t command, PrStringView rest, uint8_t terminator) {
    switch (command) {
        case 9:
            if (!isConEmuCommand(rest))
                post(vterm, PrStringView{}, rest, PrVtermNotificationCondition_None);
            break;
        case 99:
            kittyCommand(vterm, rest, terminator);
            break;
        case 777: {
            if (!(splitAt(&rest, ';') == "notify"))
                break;
            PrStringView title = splitAt(&rest, ';');
            post(vterm, title, rest, PrVtermNotificationCondition_None);
            break;
        }
        default:
            break;
    }
}

uint32_t vtermNotificationsTake(VtermNotifications* notifications, PrVtermNotification* PR_NULLABLE out,
        uint32_t capacity) {
    resetTexts(&notifications->taken);
    uint32_t count = notifications->queue.count < capacity ? notifications->queue.count : capacity;
    // A short string stores its bytes inline, so the views point into the taken records. The records stay until the
    // next take.
    notifications->taken.append(notifications->queue.data, count);
    notifications->queue.erase(0, count);
    for (uint32_t i = 0; i < count; ++i) {
        const VtermNotificationRecord& notification = notifications->taken.data[i];
        PrStringView text = prRawStringView(&notification.text);
        out[i] = PrVtermNotification{
            .title = PrStringView{text.data, notification.titleSize},
            .body = PrStringView{text.data + notification.titleSize, text.size - notification.titleSize},
            .condition = notification.condition,
        };
    }
    return count;
}

} // namespace pr_impl
