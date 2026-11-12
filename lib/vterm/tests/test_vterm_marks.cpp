// Tests the semantic prompt marks of shell integration (OSC 133) on a terminal's lines, and the shell state that the
// terminal tracks.

#include "pr/string.h"
#include "pr/string_ops.h"
#include "pr/vterm.h"

#include "tools/harness/test.h"

namespace pr_impl {

using namespace pr;

namespace {

// A terminal under test, with helpers that read the marks of its lines.
struct Term {
    PrVterm* vterm;

    explicit Term(uint32_t columns, uint32_t rows, uint32_t scrollback = 100) {
        PrVtermDesc desc{.extent = {columns, rows}, .scrollback = scrollback};
        vterm = prVtermCreate(&desc);
    }
    ~Term() { prVtermDestroy(vterm); }
    Term(const Term&) = delete;
    Term& operator=(const Term&) = delete;

    void write(PrStringView text) {
        prVtermWrite(vterm, prStringBytes(text));
    }

    uint32_t scrollback() const { return prVtermScrollbackCount(vterm); }

    // Line `index`, counted from the oldest line of the scrollback, and screen row `row`.
    PrVtermLine line(uint32_t index) const { return prVtermLine(vterm, index); }
    PrVtermLine row(uint32_t row) const { return line(scrollback() + row); }

    String text(uint32_t index) const {
        PrVtermLine at = line(index);
        PrRawString text = prRawStringNull();
        prVtermLineAppendText(vterm, index, 0, at.cellCount, &text);
        PrStringView view = prRawStringView(&text);
        while (!view.isEmpty() && view.data[view.size - 1] == ' ')
            --view.size;
        String out(view);
        prRawStringReset(&text);
        return out;
    }

    PrVtermShellInfo shell() const { return prVtermShellInfo(vterm); }
};

// Returns what a shell writes for the prompt `text`: the prompt-start mark, the prompt, and the input-start mark.
String prompt(PrStringView text) {
    return format("\x1b]133;A\x07{}\x1b]133;B\x07", text);
}

// Returns what a shell writes when it runs the typed command `command`: the echo of the input, the output-start mark,
// `output`, and the command-end mark with the exit code `exit`.
String run(PrStringView command, PrStringView output, PrStringView exit) {
    return format("{}\r\n\x1b]133;C\x07{}\x1b]133;D;{}\x07", command, output, exit);
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("vterm marks: a prompt, its input, the output and the end mark their lines, and the exit code the prompt's") {
    Term t(20, 6);
    CHECK(t.shell().state == PrVtermShellState_None);
    t.write(prompt("$ "));
    CHECK(t.shell().state == PrVtermShellState_Input);
    t.write("ls\r\n\x1b]133;C\x07");
    CHECK(t.shell().state == PrVtermShellState_Running);
    CHECK(t.row(0).commandStatus == PrVtermCommandStatus_Running);
    t.write("a\r\nb\r\n\x1b]133;D;0\x07");
    t.write(prompt("$ "));

    PrVtermLine first = t.row(0);
    CHECK(first.marks == (PrVtermLineMarkFlag_Prompt | PrVtermLineMarkFlag_Input));
    CHECK(first.inputColumn == 2);
    CHECK(first.commandStatus == PrVtermCommandStatus_Succeeded);
    CHECK(first.exitCode == 0);
    CHECK(t.row(1).marks == PrVtermLineMarkFlag_Output);
    CHECK(t.row(1).commandStatus == PrVtermCommandStatus_None);
    CHECK(t.row(2).marks == 0);
    CHECK(t.row(3).marks == (PrVtermLineMarkFlag_End | PrVtermLineMarkFlag_Prompt | PrVtermLineMarkFlag_Input));
    CHECK(t.row(3).commandStatus == PrVtermCommandStatus_None);

    PrVtermShellInfo shell = t.shell();
    CHECK(shell.state == PrVtermShellState_Input);
    CHECK(shell.lastStatus == PrVtermCommandStatus_Succeeded);
    CHECK(shell.lastExitCode == 0);
}

TEST_CASE("vterm marks: a failure keeps its exit code, an end without one is finished, and an empty command is none") {
    Term t(20, 8);
    t.write(format("{}{}{}{}{}", prompt("$ "), run("false", "", "1"), prompt("$ "), run("true", "", ""), prompt("$ ")));
    t.write(format("\r\n\x1b]133;D\x07{}", prompt("$ ")));
    CHECK(t.row(0).commandStatus == PrVtermCommandStatus_Failed);
    CHECK(t.row(0).exitCode == 1);
    CHECK(t.row(1).commandStatus == PrVtermCommandStatus_Finished);
    CHECK(t.row(2).commandStatus == PrVtermCommandStatus_None);
    CHECK(t.row(3).marks == (PrVtermLineMarkFlag_End | PrVtermLineMarkFlag_Prompt | PrVtermLineMarkFlag_Input));
    // The empty command does not change the last status, which the last command that ran set.
    CHECK(t.shell().lastStatus == PrVtermCommandStatus_Finished);

    t.write(run("exit", "", "-2"));
    CHECK(t.row(3).commandStatus == PrVtermCommandStatus_Failed);
    CHECK(t.row(3).exitCode == -2);
    CHECK(t.shell().state == PrVtermShellState_None);
    CHECK(t.shell().lastExitCode == -2);
}

TEST_CASE("vterm marks: a prompt starts on a fresh line, unless it is an explicit prompt, which starts at the cursor") {
    Term t(20, 6);
    t.write(format("{}printf x\r\n\x1b]133;C\x07x\x1b]133;D;0\x07{}", prompt("$ "), prompt("$ ")));
    CHECK(t.text(t.scrollback() + 1) == "x");
    CHECK(t.row(1).marks == (PrVtermLineMarkFlag_Output | PrVtermLineMarkFlag_End));
    CHECK(t.row(2).marks == (PrVtermLineMarkFlag_Prompt | PrVtermLineMarkFlag_Input));
    CHECK(t.text(t.scrollback() + 2) == "$");

    t.write("\r\n\x1b]133;D\x07y\x1b]133;P\x07> ");
    CHECK(t.text(t.scrollback() + 3) == "y>");
    CHECK((t.row(3).marks & PrVtermLineMarkFlag_Prompt) != 0);
}

TEST_CASE("vterm marks: a secondary prompt continues the command, and a right prompt marks nothing") {
    Term t(20, 6);
    t.write(format("{}echo 'a\r\n\x1b]133;A;k=s\x07> \x1b]133;B\x07" "b'", prompt("$ ")));
    t.write("\x1b]133;P;k=r\x07");
    CHECK(t.row(1).marks == (PrVtermLineMarkFlag_Continuation | PrVtermLineMarkFlag_Input));
    CHECK(t.row(1).inputColumn == 2);
    t.write("\r\n\x1b]133;C\x07" "a\r\nb\r\n\x1b]133;D;3\x07");
    CHECK(t.row(0).commandStatus == PrVtermCommandStatus_Failed);
    CHECK(t.row(0).exitCode == 3);
    CHECK(t.row(1).commandStatus == PrVtermCommandStatus_None);
}

TEST_CASE("vterm marks: options after a mark are read, and a mark that the terminal does not know is ignored") {
    Term t(20, 6);
    t.write("\x1b]133;A;aid=42;cl=m\x07$ \x1b]133;B\x07" "x\r\n\x1b]133;C;cmdline=x\x07\x1b]133;D;7;aid=42\x07");
    t.write("\x1b]133;Z\x07\x1b]133\x07");
    CHECK(t.row(0).marks == (PrVtermLineMarkFlag_Prompt | PrVtermLineMarkFlag_Input));
    CHECK(t.row(0).commandStatus == PrVtermCommandStatus_Failed);
    CHECK(t.row(0).exitCode == 7);
}

TEST_CASE("vterm marks: a prompt in the scrollback takes the exit code of its command") {
    Term t(20, 4);
    String output;
    for (int i = 0; i < 10; ++i)
        formatTo(output, "{}\r\n", i);
    t.write(format("{}{}{}", prompt("$ "), run("seq", output, "5"), prompt("$ ")));
    REQUIRE(t.scrollback() > 0);
    CHECK(t.line(0).marks == (PrVtermLineMarkFlag_Prompt | PrVtermLineMarkFlag_Input));
    CHECK(t.line(0).commandStatus == PrVtermCommandStatus_Failed);
    CHECK(t.line(0).exitCode == 5);
    CHECK(t.line(1).marks == PrVtermLineMarkFlag_Output);
}

TEST_CASE("vterm marks: a prompt that left the scrollback leaves the exit code to the shell's state alone") {
    Term t(20, 4, 2);
    String output;
    for (int i = 0; i < 10; ++i)
        formatTo(output, "{}\r\n", i);
    t.write(format("{}{}", prompt("$ "), run("seq", output, "0")));
    for (uint32_t i = 0; i < t.scrollback() + 4; ++i)
        CHECK(t.line(i).commandStatus == PrVtermCommandStatus_None);
    CHECK(t.shell().lastStatus == PrVtermCommandStatus_Succeeded);
}

TEST_CASE("vterm marks: the marks follow their text through a rewrap, and the prompt still takes its exit code") {
    Term t(10, 6);
    // A prompt of nine cells. The input starts at column 9 and wraps onto the next line.
    t.write(format("{}echo hi\r\n\x1b]133;C\x07hi\r\n", prompt("~/abc/d$ ")));
    REQUIRE(t.row(0).inputColumn == 9);
    prVtermResize(t.vterm, PrUVec2{5, 6});
    // After the rewrap, the lines are "~/abc", "/d$ e", "cho h" and "i", followed by the output.
    uint32_t prompt = 0;
    while (prompt < t.scrollback() + 6 && t.text(prompt) != "~/abc")
        ++prompt;
    REQUIRE(prompt < t.scrollback() + 6);
    CHECK(t.line(prompt).marks == PrVtermLineMarkFlag_Prompt);
    CHECK(t.line(prompt + 1).marks == PrVtermLineMarkFlag_Input);
    CHECK(t.line(prompt + 1).inputColumn == 4);
    CHECK(t.line(prompt + 4).marks == PrVtermLineMarkFlag_Output);
    CHECK(t.text(prompt + 4) == "hi");

    prVtermResize(t.vterm, PrUVec2{20, 6});
    t.write("\x1b]133;D;9\x07");
    uint32_t joined = 0;
    while (joined < t.scrollback() + 6 && (t.line(joined).marks & PrVtermLineMarkFlag_Prompt) == 0)
        ++joined;
    REQUIRE(joined < t.scrollback() + 6);
    CHECK(t.text(joined) == "~/abc/d$ echo hi");
    CHECK(t.line(joined).marks == (PrVtermLineMarkFlag_Prompt | PrVtermLineMarkFlag_Input));
    CHECK(t.line(joined).inputColumn == 9);
    CHECK(t.line(joined).commandStatus == PrVtermCommandStatus_Failed);
    CHECK(t.line(joined).exitCode == 9);
    CHECK(t.line(joined + 1).marks == PrVtermLineMarkFlag_Output);
}

TEST_CASE("vterm marks: an erase of the whole line clears its marks, and an erase of part of it keeps them") {
    Term t(20, 6);
    t.write(format("{}abc", prompt("$ ")));
    uint64_t revision = t.row(0).revision;
    t.write("\x1b[3D\x1b[K");
    CHECK(t.row(0).marks == (PrVtermLineMarkFlag_Prompt | PrVtermLineMarkFlag_Input));
    t.write("\r\x1b[K");
    CHECK(t.row(0).marks == 0);
    CHECK(t.row(0).revision != revision);
    // The shell writes its prompt again, as it does on a redraw.
    t.write(format("{}x\r\n\x1b]133;C\x07" "out\r\n", prompt("$ ")));
    t.write("\x1b[2J");
    for (uint32_t row = 0; row < 6; ++row)
        CHECK(t.row(row).marks == 0);
    // The prompt is gone, so the end of its command updates only the shell state.
    t.write("\x1b]133;D;1\x07");
    CHECK(t.shell().lastStatus == PrVtermCommandStatus_Failed);
    CHECK(t.row(0).commandStatus == PrVtermCommandStatus_None);
}

TEST_CASE("vterm marks: the prompt that a shell writes again on its line keeps the line's command") {
    Term t(20, 6);
    t.write(format("{}ab", prompt("$ ")));
    t.write(format("\r{}ab\r\n\x1b]133;C\x07\x1b]133;D;0\x07", prompt("$ ")));
    CHECK(t.row(0).commandStatus == PrVtermCommandStatus_Succeeded);
}

TEST_CASE("vterm marks: a full reset clears the marks and the shell's state") {
    Term t(20, 6);
    t.write(format("{}{}{}", prompt("$ "), run("true", "", "0"), prompt("$ ")));
    t.write("\x1b" "c");
    CHECK(t.row(0).marks == 0);
    CHECK(t.shell().state == PrVtermShellState_None);
    CHECK(t.shell().lastStatus == PrVtermCommandStatus_None);
    t.write(prompt("$ "));
    prVtermReset(t.vterm);
    CHECK(t.row(0).marks == 0);
    CHECK(t.shell().state == PrVtermShellState_None);
}

TEST_CASE("vterm marks: the alternate screen takes no marks") {
    Term t(20, 6);
    t.write(format("{}vi\r\n\x1b]133;C\x07\x1b[?1049h{}", prompt("$ "), prompt("> ")));
    CHECK(t.row(0).marks == 0);
    CHECK(t.shell().state == PrVtermShellState_Running);
    t.write("\x1b[?1049l\x1b]133;D;0\x07");
    CHECK(t.row(0).commandStatus == PrVtermCommandStatus_Succeeded);
}
