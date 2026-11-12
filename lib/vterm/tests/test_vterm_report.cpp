// Tests the reports of the terminal's settings, capabilities and color scheme that a program requests.

#include "pr/string.h"
#include "pr/string_ops.h"
#include "pr/vterm.h"

#include "tools/harness/test.h"

namespace pr_impl {

using namespace pr;

namespace {

// A terminal under test, with helpers that send requests and read the answers.
struct Term {
    PrVterm* vterm;

    explicit Term(uint32_t columns = 10, uint32_t rows = 5) {
        PrVtermDesc desc{.extent = {columns, rows}, .scrollback = 10};
        vterm = prVtermCreate(&desc);
    }
    ~Term() { prVtermDestroy(vterm); }
    Term(const Term&) = delete;
    Term& operator=(const Term&) = delete;

    void write(PrStringView text) {
        prVtermWrite(vterm, prStringBytes(text));
    }

    // Returns and consumes the output queued for the program.
    String output() {
        PrByteView bytes = prVtermOutput(vterm);
        String out(prStringFromBytes(bytes));
        prVtermOutputConsume(vterm, bytes.size);
        return out;
    }

    // The answer to a request for the setting of the control function `name` (DECRQSS).
    String setting(PrStringView name) {
        write(format("\x1bP$q{}\x1b\\", name));
        return output();
    }

    // The answer to a request for the capabilities `names`, separated by semicolons (XTGETTCAP).
    String capabilities(PrStringView names) {
        String hex;
        for (char c : names) {
            if (c == ';')
                hex.append(c);
            else
                hex.append(toHex(PrStringView(&c, 1)));
        }
        write(format("\x1bP+q{}\x1b\\", hex));
        return output();
    }

    static String toHex(PrStringView text) {
        static const char kDigits[] = "0123456789ABCDEF";
        String out;
        for (char c : text) {
            out.append(kDigits[uint8_t(c) >> 4]);
            out.append(kDigits[uint8_t(c) & 15]);
        }
        return out;
    }
};

// The reply to a request for a setting that the terminal has.
String settingReply(PrStringView setting) {
    return format("\x1bP1$r{}\x1b\\", setting);
}

// The reply for a capability that the terminal has. A boolean capability has no value.
String capabilityReply(PrStringView name, const PrStringView* value = nullptr) {
    String reply = format("\x1bP1+r{}", Term::toHex(name));
    if (value != nullptr)
        formatTo(reply, "={}", Term::toHex(*value));
    return format("{}\x1b\\", reply);
}

String capabilityReply(PrStringView name, PrStringView value) {
    return capabilityReply(name, &value);
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("vterm reports: DECRQSS reports the pen as SGR, starting from a reset") {
    Term term;
    CHECK(term.setting("m") == settingReply("0m"));
    term.write("\x1b[1;2;3;5;7;8;9;53m");
    CHECK(term.setting("m") == settingReply("0;1;2;3;5;7;8;9;53m"));
    term.write("\x1b[0;4m");
    CHECK(term.setting("m") == settingReply("0;4m"));
    term.write("\x1b[4:3m");
    CHECK(term.setting("m") == settingReply("0;4:3m"));
    term.write("\x1b[21m");
    CHECK(term.setting("m") == settingReply("0;4:2m"));
    term.write("\x1b[24;4:5m");
    CHECK(term.setting("m") == settingReply("0;4:5m"));
}

TEST_CASE("vterm reports: DECRQSS reports colors in their shortest forms, and extended colors with colons") {
    Term term;
    term.write("\x1b[31;42m");
    CHECK(term.setting("m") == settingReply("0;31;42m"));
    term.write("\x1b[91;102m");
    CHECK(term.setting("m") == settingReply("0;91;102m"));
    term.write("\x1b[38;5;196;48;2;1;2;3m");
    CHECK(term.setting("m") == settingReply("0;38:5:196;48:2::1:2:3m"));
    term.write("\x1b[0;58:2::10:20:30m");
    CHECK(term.setting("m") == settingReply("0;58:2::10:20:30m"));
    term.write("\x1b[58;5;4m");
    CHECK(term.setting("m") == settingReply("0;58:5:4m"));
    term.write("\x1b[59;4:3;38:2::255:128:0m");
    CHECK(term.setting("m") == settingReply("0;4:3;38:2::255:128:0m"));
}

TEST_CASE("vterm reports: DECRQSS reports the scroll region, the cursor's style and the conformance level") {
    Term term(10, 6);
    CHECK(term.setting("r") == settingReply("1;6r"));
    term.write("\x1b[2;4r");
    CHECK(term.setting("r") == settingReply("2;4r"));

    // The left and right margins are the screen's edges until DECLRMM lets DECSLRM set them.
    CHECK(term.setting("s") == settingReply("1;10s"));
    term.write("\x1b[?69h\x1b[3;7s");
    CHECK(term.setting("s") == settingReply("3;7s"));
    term.write("\x1b[?69l");
    CHECK(term.setting("s") == settingReply("1;10s"));

    // A steady block is the default.
    CHECK(term.setting(" q") == settingReply("2 q"));
    term.write("\x1b[6 q");
    CHECK(term.setting(" q") == settingReply("6 q"));
    term.write("\x1b[3 q");
    CHECK(term.setting(" q") == settingReply("3 q"));
    term.write("\x1b[2 q");
    CHECK(term.setting(" q") == settingReply("2 q"));
    term.write("\x1b[0 q");
    CHECK(term.setting(" q") == settingReply("1 q"));

    // Level 2 (the VT200 series) with 7-bit controls, which matches the primary device attributes.
    CHECK(term.setting("\"p") == settingReply("62;1\"p"));
}

TEST_CASE("vterm reports: DECRQSS answers a setting that the terminal does not have as invalid") {
    Term term;
    CHECK(term.setting("x") == "\x1bP0$r\x1b\\");
    CHECK(term.setting("") == "\x1bP0$r\x1b\\");
    CHECK(term.setting("mm") == "\x1bP0$r\x1b\\");
}

TEST_CASE("vterm reports: XTGETTCAP answers each capability that the terminal has, and each other as invalid") {
    Term term;
    CHECK(term.capabilities("TN") == capabilityReply("TN", "xterm-256color"));
    CHECK(term.capabilities("Co") == capabilityReply("Co", "256"));
    CHECK(term.capabilities("colors") == capabilityReply("colors", "256"));
    CHECK(term.capabilities("RGB") == capabilityReply("RGB"));
    CHECK(term.capabilities("Tc") == capabilityReply("Tc"));
    CHECK(term.capabilities("Smulx") == capabilityReply("Smulx", "\x1b[4:%p1%dm"));
    CHECK(term.capabilities("Ss") == capabilityReply("Ss", "\x1b[%p1%d q"));
    CHECK(term.capabilities("Se") == capabilityReply("Se", "\x1b[2 q"));
    CHECK(term.capabilities("Ms") == capabilityReply("Ms", "\x1b]52;%p1%s;%p2%s\x07"));
    CHECK(term.capabilities("nope") == "\x1bP0+r\x1b\\");

    // Each name gets its own reply, in the order of the request.
    CHECK(term.capabilities("Co;nope;Tc") ==
        format("{}\x1bP0+r\x1b\\{}", capabilityReply("Co", "256"), capabilityReply("Tc")));

    // A name in lowercase hexadecimal is also accepted, and the reply spells it as the request did.
    term.write("\x1bP+q544e\x1b\\");
    CHECK(term.output() == format("\x1bP1+r544e={}\x1b\\", Term::toHex("xterm-256color")));

    // A name that is not hexadecimal is invalid.
    term.write("\x1bP+q54G\x1b\\\x1bP+q545\x1b\\");
    CHECK(term.output() == "\x1bP0+r\x1b\\\x1bP0+r\x1b\\");
}

TEST_CASE("vterm reports: XTGETTCAP reports the sequences of the extensions that the terminal implements") {
    Term term;
    CHECK(term.capabilities("Setulc") ==
        capabilityReply("Setulc", "\x1b[58:2::%p1%{65536}%/%d:%p1%{256}%/%{255}%&%d:%p1%{255}%&%d%;m"));
    CHECK(term.capabilities("Sync") == capabilityReply("Sync", "\x1b[?2026%?%p1%{1}%-%tl%eh%;"));
    CHECK(term.capabilities("BE;BD") == format("{}{}", capabilityReply("BE", "\x1b[?2004h"), capabilityReply("BD", "\x1b[?2004l")));
}

TEST_CASE("vterm reports: the color scheme is dark until the caller sets it, and a query reports it") {
    Term term;
    CHECK(prVtermColorScheme(term.vterm) == PrAppColorScheme_Dark);
    term.write("\x1b[?996n");
    CHECK(term.output() == "\x1b[?997;1n");
    prVtermSetColorScheme(term.vterm, PrAppColorScheme_Light);
    CHECK(prVtermColorScheme(term.vterm) == PrAppColorScheme_Light);
    // Without mode 2031, the change is not reported.
    CHECK(term.output().isEmpty());
    term.write("\x1b[?996n");
    CHECK(term.output() == "\x1b[?997;2n");

    // A reset by the program keeps the scheme that the caller set.
    term.write("\x1b" "c\x1b[?996n");
    CHECK(term.output() == "\x1b[?997;2n");
}

TEST_CASE("vterm reports: mode 2031 reports each change of the color scheme, and DECRQM reports the mode") {
    Term term;
    term.write("\x1b[?2031$p");
    CHECK(term.output() == "\x1b[?2031;2$y");
    term.write("\x1b[?2031h\x1b[?2031$p");
    CHECK(term.output() == "\x1b[?2031;1$y");

    prVtermSetColorScheme(term.vterm, PrAppColorScheme_Light);
    CHECK(term.output() == "\x1b[?997;2n");
    // Setting the scheme that the terminal already has is not a change.
    prVtermSetColorScheme(term.vterm, PrAppColorScheme_Light);
    CHECK(term.output().isEmpty());
    prVtermSetColorScheme(term.vterm, PrAppColorScheme_Dark);
    CHECK(term.output() == "\x1b[?997;1n");

    term.write("\x1b[?2031l");
    prVtermSetColorScheme(term.vterm, PrAppColorScheme_Light);
    CHECK(term.output().isEmpty());

    // A full reset ends the mode.
    term.write("\x1b[?2031h\x1b" "c");
    prVtermSetColorScheme(term.vterm, PrAppColorScheme_Dark);
    CHECK(term.output().isEmpty());
}
