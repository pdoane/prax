// Strings as values: text that a string owns, copies when it is copied, and frees when it goes. An implementation in
// lib holds a PrRawString (pr/raw_string.h) and resets it (lint.py, `managed-include`).

#pragma once

#include "pr/assert.h"
#include "pr/base.h"
#include "pr/format.h"
#include "pr/hash_key.h"
#include "pr/raw_string.h"
#include "pr/string_view.h"
#include "pr/writer.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr {

// A string that owns its text, as one PrRawString: inline if it fits, and on the heap otherwise. Text and a string
// convert to each other implicitly: a string passes wherever the C API takes a PrStringView, and text assigns to a
// string by copying. A string keeps the null view apart from the empty view, as PrRawString does: a string made with no
// text, or from the null view, is null, and one made from an empty view is empty.
struct String {
    // A table keyed by strings looks them up by their views (pr/hash_key.h).
    using Lookup = PrStringView;

    String() = default;
    String(PrStringView text) : string(prRawStringCopy(text)) {}
    String(const char* PR_NULLABLE text) : string(prRawStringCopy(PrStringView(text))) {}

    String(const String& other) : string(prRawStringCopy(other.view())) {}
    String(String&& other) : string(other.string) { other.string = PrRawString{}; }

    String& operator=(const String& other) {
        if (this != &other)
            assign(other.view());
        return *this;
    }
    String& operator=(String&& other) {
        if (this != &other) {
            prRawStringReset(&string);
            string = other.string;
            other.string = PrRawString{};
        }
        return *this;
    }

    // Replace the text with `text`, as assign() does. A NUL-terminated string has an overload of its own, since it
    // converts as well to a String as to a view.
    String& operator=(PrStringView text) { return assign(text); }
    String& operator=(const char* PR_NULLABLE text) { return assign(PrStringView(text)); }

    ~String() { prRawStringReset(&string); }

    // Returns a string that takes `owned`, which the string then frees.
    static String adopt(PrRawString owned) {
        String out;
        out.string = owned;
        return out;
    }

    // Returns the PrRawString, which the caller then owns. The string is left empty.
    PrRawString release() {
        PrRawString out = string;
        string = PrRawString{};
        return out;
    }

    // The text, and how many bytes it has, as Array counts its elements. A string holds fewer than 2^32 bytes, which
    // PrRawString asserts.
    PrStringView view() const PR_LIFETIMEBOUND { return prRawStringView(&string); }
    operator PrStringView() const PR_LIFETIMEBOUND { return view(); }
    const char* PR_NULLABLE data() const PR_LIFETIMEBOUND { return view().data; }
    uint32_t count() const { return uint32_t(view().size); }

    // Return true if the string is null, or if it holds no bytes. A null string holds none.
    bool isNull() const { return prRawStringIsNull(&string); }
    bool isEmpty() const { return view().size == 0; }
    const char* PR_NULLABLE begin() const { return view().begin(); }
    const char* PR_NULLABLE end() const { return view().end(); }

    // Replaces the text with `text`, which may be the string's own, and keeps the storage where it fits. The null view
    // makes the string null.
    String& assign(PrStringView text) {
        if (holds(text)) {
            prRawStringReplace(&string, text);
        } else if (text.isNull()) {
            prRawStringReset(&string);
        } else {
            prRawStringClear(&string);
            prRawStringAppend(&string, text);
        }
        return *this;
    }

    // Appends `text`, which may be the string's own, or the byte `c`.
    String& append(PrStringView text) {
        if (holds(text)) {
            String copy(text);
            prRawStringAppend(&string, copy.view());
        } else {
            prRawStringAppend(&string, text);
        }
        return *this;
    }
    String& append(char c) { return append(PrStringView{&c, 1}); }

    // Appends `count` copies of the byte `c`.
    String& append(uint32_t count, char c) {
        char run[64];
        for (char& slot : run)
            slot = c;
        while (count > 0) {
            uint32_t step = count < sizeof(run) ? count : uint32_t(sizeof(run));
            prRawStringAppend(&string, PrStringView{run, step});
            count -= step;
        }
        return *this;
    }

    // Returns the last byte. The string must not be empty.
    char last() const {
        PrStringView held = view();
        PR_ASSERT(held.size > 0);
        return held.data[held.size - 1];
    }

    // Replaces the `replacedCount` bytes from `start`, which must lie within the string, with `text`, which may be the
    // string's own.
    String& replace(uint32_t start, uint32_t replacedCount, PrStringView text) {
        PrStringView held = view();
        PR_ASSERT(start <= held.size && replacedCount <= held.size - start);
        String out(PrStringView{held.data, start});
        out.append(text);
        out.append(PrStringView{held.data + start + replacedCount, held.size - start - replacedCount});
        return *this = move(out);
    }

    // Empties the string and keeps its storage.
    void clear() { prRawStringClear(&string); }

    // Frees the storage and leaves the string empty.
    void reset() { prRawStringReset(&string); }

    friend bool operator==(const String& a, const String& b) { return a.view() == b.view(); }
    friend bool operator==(const String& a, PrStringView b) { return a.view() == b; }
    friend bool operator==(const String& a, const char* PR_NULLABLE b) { return a.view() == PrStringView(b); }

private:
    PrRawString string = {};

    // Returns true if `text` points into the string's own bytes.
    bool holds(PrStringView text) const {
        PrStringView held = view();
        if (text.isEmpty() || held.isEmpty())
            return false;
        uintptr_t at = uintptr_t(text.data);
        return at >= uintptr_t(held.data) && at < uintptr_t(held.data + held.size);
    }
};

// Returns the hash of the text of `key`, which is the hash of a view of it.
inline uint32_t hashKey(const String& key) {
    return hashKey(key.view());
}

// Writes the text of `value`.
inline void formatValue(Writer& writer, const String& value, const FormatSpec& spec) {
    formatText(writer, value.view(), spec);
}

// A writer that appends to a String, through a buffer of its own. The text reaches the string when the writer is
// flushed, which its destructor does.
struct StringWriter : Writer {
    explicit StringWriter(String* target) :
            Writer{.drain = appendTo, .context = target, .buffer = storage, .capacity = sizeof(storage)} {}

    ~StringWriter() { flush(); }
    StringWriter(const StringWriter&) = delete;
    StringWriter& operator=(const StringWriter&) = delete;

private:
    char storage[256];

    static bool appendTo(void* PR_NULLABLE context, PrStringView text) {
        static_cast<String*>(context)->append(text);
        return true;
    }
};

// Appends `format` with its fields replaced by `arguments` to `out`.
template <typename... Arguments>
void formatTo(String& out, PrStringView format, const Arguments&... arguments) {
    StringWriter writer(&out);
    formatTo(writer, format, arguments...);
}

// Returns `format` with its fields replaced by `arguments`.
template <typename... Arguments>
String format(PrStringView format, const Arguments&... arguments) {
    String out;
    formatTo(out, format, arguments...);
    return out;
}

// Returns the texts of `parts` one after another, as `concat(directory, "/", name)`. Unlike format, it reads no fields,
// so a part may hold braces.
template <typename... Parts>
String concat(const Parts&... parts) {
    String out;
    (out.append(PrStringView(parts)), ...);
    return out;
}

} // namespace pr

PR_ASSUME_NONNULL_END
