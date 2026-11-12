// Tests the notifications that a program posts: OSC 9, OSC 777, and OSC 99 with its chunks, encodings and conditions.

#include "pr/array.h"
#include "pr/string.h"
#include "pr/string_ops.h"
#include "pr/vterm.h"

#include "tools/harness/test.h"

namespace pr_impl {

using namespace pr;

namespace {

// A notification as a test reads it.
struct Posted {
    String title;
    String body;
    PrVtermNotificationCondition condition;

    bool operator==(const Posted& other) const {
        return title == other.title && body == other.body && condition == other.condition;
    }
};

// A terminal under test, with a helper that takes what its program posted.
struct Term {
    PrVterm* vterm;

    Term() {
        PrVtermDesc desc{.extent = {40, 10}, .scrollback = 10};
        vterm = prVtermCreate(&desc);
    }
    ~Term() { prVtermDestroy(vterm); }
    Term(const Term&) = delete;
    Term& operator=(const Term&) = delete;

    void write(PrStringView text) {
        prVtermWrite(vterm, prStringBytes(text));
    }

    // Takes every pending notification.
    Array<Posted> take() {
        Array<Posted> out;
        PrVtermNotification taken[4];
        for (;;) {
            uint32_t count = prVtermTakeNotifications(vterm, taken, 4);
            if (count == 0)
                return out;
            for (uint32_t i = 0; i < count; ++i) {
                out.push(Posted{String(taken[i].title), String(taken[i].body), taken[i].condition});
            }
        }
    }

    // Returns and consumes the output queued for the program.
    String output() {
        PrByteView bytes = prVtermOutput(vterm);
        String out(prStringFromBytes(bytes));
        prVtermOutputConsume(vterm, bytes.size);
        return out;
    }
};

using Notifications = Array<Posted>;

constexpr PrVtermNotificationCondition kNone = PrVtermNotificationCondition_None;

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("vterm: OSC 9 posts its text as a body, and ConEmu's numbered forms post nothing") {
    Term t;
    t.write("\x1b]9;Build finished; 3 warnings\x07");
    CHECK((prVtermTakeEvents(t.vterm) & PrVtermEventFlag_Notification) != 0);
    CHECK(t.take() == Notifications({{"", "Build finished; 3 warnings", kNone}}));
    t.write("\x1b]9;4;1;50\x1b\\\x1b]9;9;/tmp\x07\x1b]9;12\x07\x1b]9;\x07");
    CHECK((prVtermTakeEvents(t.vterm) & PrVtermEventFlag_Notification) == 0);
    CHECK(t.take().isEmpty());
    // A number that names no ConEmu command, or a number followed by text, is posted as text.
    t.write("\x1b]9;42\x07\x1b]9;5 files copied\x07");
    CHECK(t.take() == Notifications({{"", "42", kNone}, {"", "5 files copied", kNone}}));
}

TEST_CASE("vterm: OSC 777 notify posts a title and a body, and its other commands post nothing") {
    Term t;
    t.write("\x1b]777;notify;Make;done; all targets\x07\x1b]777;precmd\x07");
    CHECK(t.take() == Notifications({{"Make", "done; all targets", kNone}}));
}

TEST_CASE("vterm: OSC 99 posts a title, and with p=body a body, in chunks that end without d=0") {
    Term t;
    t.write("\x1b]99;;Hello world\x1b\\");
    CHECK(t.take() == Notifications({{"Hello world", "", kNone}}));
    prVtermTakeEvents(t.vterm);
    t.write("\x1b]99;i=1:d=0;Hello\x1b\\");
    t.write("\x1b]99;i=1:d=0; world\x1b\\");
    CHECK(t.take().isEmpty());
    CHECK((prVtermTakeEvents(t.vterm) & PrVtermEventFlag_Notification) == 0);
    t.write("\x1b]99;i=1:p=body;This is the body\x1b\\");
    CHECK(t.take() == Notifications({{"Hello world", "This is the body", kNone}}));
}

TEST_CASE("vterm: OSC 99 decodes each chunk of a base64 payload on its own") {
    Term t;
    // "Résumé" in two chunks that split its first é, then a body of "ok".
    t.write("\x1b]99;i=a:d=0:e=1;UsM=\x1b\\");
    t.write("\x1b]99;i=a:d=0:e=1;qXN1bcOp\x1b\\");
    t.write("\x1b]99;i=a:e=1:p=body;b2s=\x1b\\");
    CHECK(t.take() == Notifications({{"R\xc3\xa9sum\xc3\xa9", "ok", kNone}}));
    // A payload that is not base64 drops the notification.
    t.write("\x1b]99;e=1;!!!\x1b\\");
    CHECK(t.take().isEmpty());
}

TEST_CASE("vterm: OSC 99 reads the condition, and a chunk of another identifier drops the one in progress") {
    Term t;
    t.write("\x1b]99;o=unfocused;A\x1b\\\x1b]99;o=invisible;B\x1b\\\x1b]99;o=always;C\x1b\\\x1b]99;o=sometimes;D\x1b\\");
    CHECK(t.take() == Notifications({
        {"A", "", PrVtermNotificationCondition_Unfocused},
        {"B", "", PrVtermNotificationCondition_Invisible},
        {"C", "", PrVtermNotificationCondition_Always},
        {"D", "", kNone},
    }));
    t.write("\x1b]99;i=x:d=0;lost\x1b\\\x1b]99;i=y;kept\x1b\\\x1b]99;i=x;alone\x1b\\");
    CHECK(t.take() == Notifications({{"kept", "", kNone}, {"alone", "", kNone}}));
}

TEST_CASE("vterm: OSC 99's query is answered with what the terminal supports, with the program's terminator") {
    Term t;
    t.write("\x1b]99;i=q:p=?;\x1b\\");
    CHECK(t.output() == "\x1b]99;i=q:p=?;a=focus:o=always,unfocused,invisible:p=title,body,?\x1b\\");
    t.write("\x1b]99;p=?;\x07");
    CHECK(t.output() == "\x1b]99;p=?;a=focus:o=always,unfocused,invisible:p=title,body,?\x07");
    CHECK(t.take().isEmpty());
}

TEST_CASE("vterm: a notification drops control characters but the body's line feeds, and is cut at its limits") {
    Term t;
    // The parser drops C0 controls from a command's text, but a base64 payload can still contain them.
    t.write("\x1b]777;notify;Ti\x7ftle;Bo\xc2\x85" "dy\x07");
    CHECK(t.take() == Notifications({{"Title", "Body", kNone}}));
    t.write("\x1b]99;e=1:p=body;b25lCnR3bw0JdGhyZWU=\x1b\\");
    CHECK(t.take() == Notifications({{"", "one\ntwothree", kNone}}));
    // Text that is not valid UTF-8 posts nothing. A notification with neither a title nor a body posts nothing.
    t.write("\x1b]9;bad \xff text\x07\x1b]777;notify;;\x07");
    CHECK(t.take().isEmpty());
    String title = String().append(kPrVtermNotificationMaxTitleBytes - 1, 'a');
    String body = String().append(kPrVtermNotificationMaxBodyBytes + 100, 'b');
    // The title's last character is two bytes long. It does not fit, so it is dropped.
    t.write(format("\x1b]777;notify;{}\xc3\xa9;{}\x07", title, body));
    Array<Posted> posted = t.take();
    REQUIRE(posted.count() == 1);
    CHECK(posted[0].title == title);
    CHECK(posted[0].body == String().append(kPrVtermNotificationMaxBodyBytes, 'b'));
}

TEST_CASE("vterm: notifications past the most drop the oldest, a take writes up to its capacity, and a reset drops them") {
    Term t;
    for (uint32_t i = 0; i < kPrVtermMaxNotifications + 2; ++i)
        t.write(format("\x1b]9;{}!\x07", i));
    PrVtermNotification one{};
    CHECK(prVtermTakeNotifications(t.vterm, &one, 1) == 1);
    CHECK(one.body == "2!");
    CHECK(t.take().count() == kPrVtermMaxNotifications - 1);
    t.write("\x1b]9;pending\x07\x1b]99;i=z:d=0;open\x1b\\");
    prVtermReset(t.vterm);
    t.write("\x1b]99;i=z;closed\x1b\\");
    CHECK(t.take() == Notifications({{"closed", "", kNone}}));
}
