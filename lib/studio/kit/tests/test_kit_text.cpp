// Studio Kit's text fields and areas as they are edited: typing, the caret's motions, the selection, the pointer, the
// clipboard, and what the layout tree paints for the selection and the caret.

#include "pr/app_backend.h"
#include "pr/kit_box.h"
#include "pr/kit_input.h"
#include "pr/span.h"
#include "pr/string.h"
#include "pr/text_edit.h"
#include "pr/ui_text_edit.h"

#include "tools/harness/kit_fixture.h"

#include "tools/harness/test.h"

using namespace harness;

namespace pr_impl {

namespace {

// Writes the text of the control `field` to `owned`, if it changed in the round, and returns the control.
PrLayoutNode taken(PrKit* kit, PrLayoutNode field, String& owned) {
    PrStringView text{};
    if (prKitTextChanged(kit, field, &text))
        owned.assign(text);
    return field;
}

// An application whose clipboard is a string, and its window. A read waits until the test answers it.
struct ClipboardApp : PrApp {
    String text;
    PrAppDataTextFn PR_NULLABLE pending = nullptr;
    void* PR_NULLABLE pendingContext = nullptr;
    uint32_t cancels = 0;
};

struct ClipboardWindow : PrAppWindow {
    ClipboardApp* app;
};

struct Clipboard {
    ClipboardApp app;
    ClipboardWindow window;

    Clipboard() {
        app.dataReadFn = [](PrApp* a, const PrAppDataRead* read) {
            ClipboardApp* clipboard = static_cast<ClipboardApp*>(a);
            REQUIRE(read->kind == PrAppDataReadKind_Text);
            REQUIRE(clipboard->pending == nullptr);
            clipboard->pending = reinterpret_cast<PrAppDataTextFn>(read->done);
            clipboard->pendingContext = read->context;
        };
        app.cancelFn = [](PrApp* a, void* PR_NULLABLE context) {
            ClipboardApp* clipboard = static_cast<ClipboardApp*>(a);
            if (clipboard->pending != nullptr && clipboard->pendingContext == context)
                clipboard->pending = nullptr;
            clipboard->cancels++;
        };
        app.dataWriteFn = [](PrApp* a, PrAppDataSource, const PrAppDataContent* content) -> PrStatus {
            static_cast<ClipboardApp*>(a)->text.assign(content->text);
            return PrStatus_Ok;
        };
        window.app = &app;
        window.appFn = [](const PrAppWindow* w) -> PrApp* { return static_cast<const ClipboardWindow*>(w)->app; };
        window.requestFrameFn = [](PrAppWindow*) {};
    }

    // Answers the pending read with the clipboard's text, as the application does after the callback that read returns.
    void answer() {
        REQUIRE(app.pending != nullptr);
        PrAppDataTextFn done = app.pending;
        app.pending = nullptr;
        done(app.pendingContext, PrStatus_Ok, app.text);
    }
};

// A text field or area in its own column, and its text.
struct Edited {
    KitFixture f;
    String text;
    PrLayoutNode node = kPrLayoutNodeNone;

    explicit Edited(PrStringView initial, bool area = false, uint32_t rows = 3) : text(initial) {
        f.build = [this, area, rows](PrLayoutNode root) {
            PrLayoutNode column = prKitBox(f.kit, root, {}, {.place = {.width = prKitPx(200)}});
            if (area)
                node = taken(f.kit, prKitTextArea(f.kit, column, {.text = text, .rows = rows}, {}), text);
            else
                node = taken(f.kit, prKitTextField(f.kit, column, {.text = text, .placeholder = "Name"}, {}), text);
        };
        f.frame();
    }

    // Gives the control focus by a press at the end of its text, and runs frames until the layout shows it.
    void focus() {
        PrRect box = f.bounds(node);
        f.click(PrVec2{box.right - 4, box.top + 4});
        f.frame();
        f.frame();
        REQUIRE(prUiFocus(f.ui) == node);
    }

    PrUiTextSelection selection() { return prUiTextEditSelection(f.ui, node); }

    // Returns the text node that shows the text: the first text node under the control.
    PrLayoutNode textNode() {
        PrLayoutNode at = prLayoutNodeFirstChild(f.tree, node);
        while (at != kPrLayoutNodeNone && prLayoutNodeKind(f.tree, at) != PrLayoutNodeKind_Text) {
            PrLayoutNode child = prLayoutNodeFirstChild(f.tree, at);
            at = child != kPrLayoutNodeNone ? child : prLayoutNodeNextSibling(f.tree, at);
        }
        return at;
    }
};

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("typing into a text field inserts at the caret, and each typed text is one gesture") {
    Edited e("ab");
    uint32_t commits = 0;
    e.f.edit = [](void* context, PrLayoutNode, PrUiEditPhase phase) {
        *static_cast<uint32_t*>(context) += phase == PrUiEditPhase_Commit ? 1 : 0;
    };
    e.f.editContext = &commits;
    e.focus();
    CHECK(e.selection().caret == 2);
    e.f.type("cd");
    e.f.frame();
    CHECK(e.text == "abcd");
    CHECK(commits == 1);
    e.f.key(PrAppKey_Left);
    e.f.frame();
    e.f.type("X");
    e.f.frame();
    CHECK(e.text == "abcXd");
    e.f.key(PrAppKey_Backspace);
    e.f.frame();
    e.f.key(PrAppKey_Backspace);
    e.f.frame();
    CHECK(e.text == "abd");
    e.f.key(PrAppKey_Delete);
    e.f.frame();
    CHECK(e.text == "ab");
    CHECK(commits == 5);
    // Control characters typed are dropped.
    e.f.type("\t\n");
    e.f.frame();
    CHECK(e.text == "ab");
    CHECK(kitVerified(e.f.tree));
}

TEST_CASE("Shift extends the selection, typing replaces it, and Command-A selects everything") {
    Edited e("hello world");
    e.focus();
    e.f.key(PrAppKey_Left, PrAppModifierFlag_Shift | PrAppModifierFlag_Alt);
    e.f.frame();
    CHECK(e.selection().anchor == 11);
    CHECK(e.selection().caret == 6);
    e.f.type("there");
    e.f.frame();
    CHECK(e.text == "hello there");
    e.f.key(PrAppKey_Home);
    e.f.frame();
    CHECK(e.selection().caret == 0);
    e.f.key('A', PrAppModifierFlag_Super);
    e.f.frame();
    CHECK(e.selection().anchor == 0);
    CHECK(e.selection().caret == 11);
    e.f.key(PrAppKey_Backspace);
    e.f.frame();
    CHECK(e.text.isEmpty());
    CHECK(kitVerified(e.f.tree));
}

TEST_CASE("the arrows run through a field's text in order, across a change of direction") {
    // Three Latin letters and a space, then three Hebrew letters of two bytes each, then a space and a letter.
    Edited e("abc \xd7\x90\xd7\x91\xd7\x92 z");
    e.focus();
    e.f.key(PrAppKey_Home);
    e.f.frame();
    const uint32_t offsets[] = {1, 2, 3, 4, 6, 8, 10, 11, 12};
    for (uint32_t expected : offsets) {
        e.f.key(PrAppKey_Right);
        e.f.frame();
        CHECK(e.selection().caret == expected);
    }
    e.f.key(PrAppKey_Backspace);
    e.f.frame();
    CHECK(e.text == "abc \xd7\x90\xd7\x91\xd7\x92 ");
}

TEST_CASE("an edit's description chooses how its arrows move at a change of direction") {
    struct Case {
        PrTextEditArrowMotion motion;
        Span<const uint32_t> offsets;
    };
    const uint32_t logical[] = {1, 2, 3, 4, 4, 6, 8, 10, 10, 11, 12};
    const uint32_t visual[] = {1, 2, 3, 4, 8, 6, 4, 11, 12};
    const Case cases[] = {
        {PrTextEditArrowMotion_LogicalStops, Span(logical)},
        {PrTextEditArrowMotion_Visual, Span(visual)},
    };
    for (const Case& c : cases) {
        KitFixture f;
        String text = "abc \xd7\x90\xd7\x91\xd7\x92 z";
        PrLayoutNode control = kPrLayoutNodeNone;
        f.build = [&](PrLayoutNode root) {
            control = prKitBox(f.kit, root, {}, {.place = {.width = prKitPx(200)}});
            prUiAddFlags(f.ui, control, PrUiFlag_Pressable | PrUiFlag_Focusable);
            if (prUiFocus(f.ui) != control)
                prUiSetFocus(f.ui, control);
            // A label's element gives the edit's text a font.
            PrLayoutNode label = prKitLabel(f.kit, control, {.text = ""}, {});
            PrUiTextEditDesc edit{.control = control, .parent = label, .value = text, .arrowMotion = c.motion};
            prUiTextEdit(f.ui, &edit);
            PrStringView changed{};
            if (prUiTextEditChanged(f.ui, control, &changed))
                text.assign(changed);
        };
        f.frame();
        f.frame();
        REQUIRE(prUiFocus(f.ui) == control);
        f.key(PrAppKey_Home);
        f.frame();
        for (uint32_t expected : c.offsets) {
            f.key(PrAppKey_Right);
            f.frame();
            CHECK(prUiTextEditSelection(f.ui, control).caret == expected);
        }
    }
}

TEST_CASE("a text area shows a buffer's styles in their roles and tones, and edits across them") {
    KitFixture f;
    PrTextEditBuffer* buffer = prTextEditBufferCreate("plain strong");
    prTextEditBufferSetStyle(buffer, 6, 12, 1);
    const PrKitTextAreaStyle styles[] = {
        {PrKitTextRole_Body, PrKitTone_Auto},
        {PrKitTextRole_Strong, PrKitTone_Accent},
    };
    PrLayoutNode area = kPrLayoutNodeNone;
    f.build = [&](PrLayoutNode root) {
        PrLayoutNode column = prKitBox(f.kit, root, {}, {.place = {.width = prKitPx(200)}});
        area = prKitTextArea(f.kit, column, {.buffer = buffer, .rows = 3, .styles = styles, .styleCount = 2}, {});
    };
    f.frame();
    PrRect box = f.bounds(area);
    f.click(PrVec2{box.right - 4, box.bottom - 4});
    f.frame();
    f.frame();
    REQUIRE(prUiFocus(f.ui) == area);
    CHECK(prUiTextEditSelection(f.ui, area).caret == 12);
    f.type("!");
    f.frame();
    f.frame();
    CHECK(prTextEditBufferStyleAt(buffer, 12) == 1);
    PrStringView text = prTextEditBufferText(buffer);
    CHECK(text == "plain strong!");
    prTextEditBufferDestroy(buffer);
}

TEST_CASE("a text field shows a styled buffer as its plain text") {
    KitFixture f;
    PrTextEditBuffer* buffer = prTextEditBufferCreate("plain strong");
    prTextEditBufferSetStyle(buffer, 6, 12, 1);
    PrLayoutNode field = kPrLayoutNodeNone;
    f.build = [&](PrLayoutNode root) { field = prKitTextField(f.kit, root, {.buffer = buffer}, {}); };
    f.frame();
    f.frame();
    CHECK(field != kPrLayoutNodeNone);
    prTextEditBufferDestroy(buffer);
}

TEST_CASE("the caret moves over a grapheme cluster as a whole") {
    // e and a combining acute accent, then a family emoji of four people joined by zero-width joiners.
    Edited e("e\xcc\x81" "\xf0\x9f\x91\xa8\xe2\x80\x8d\xf0\x9f\x91\xa9\xe2\x80\x8d\xf0\x9f\x91\xa7");
    e.focus();
    uint32_t end = e.text.count();
    CHECK(e.selection().caret == end);
    e.f.key(PrAppKey_Left);
    e.f.frame();
    CHECK(e.selection().caret == 3);
    e.f.key(PrAppKey_Backspace);
    e.f.frame();
    // Backspace corrects the accent, and leaves the letter.
    CHECK(e.text == "e\xf0\x9f\x91\xa8\xe2\x80\x8d\xf0\x9f\x91\xa9\xe2\x80\x8d\xf0\x9f\x91\xa7");
    e.f.key(PrAppKey_Delete);
    e.f.frame();
    CHECK(e.text == "e");
}

TEST_CASE("a press places the caret at the nearer edge of the character under it, and a double press selects a word") {
    Edited e("alpha beta");
    e.focus();
    PrRect box = e.f.bounds(e.node);
    PrVec2 start{box.left + 1, (box.top + box.bottom) / 2};
    e.f.click(start);
    e.f.frame();
    CHECK(e.selection().caret == 0);
    CHECK(e.selection().anchor == 0);
    // The second word is past the middle of the text, and a double press there selects it.
    PrLayoutRect caret{};
    REQUIRE(prLayoutNodeTextCaret(e.f.tree, e.textNode(), 8, PrLayoutTextAffinity_Downstream,
        PrLayoutCoordinates_Viewport, &caret));
    PrVec2 beta{prLayoutUnitToPx(caret.left), start.y};
    e.f.click(beta);
    e.f.frame();
    e.f.click(beta, 0, 2);
    e.f.frame();
    CHECK(e.selection().anchor == 6);
    CHECK(e.selection().caret == 10);
    CHECK(kitVerified(e.f.tree));
}

TEST_CASE("cut, copy and paste go through the application's clipboard") {
    Edited e("copy me");
    Clipboard clipboard;
    prUiSetWindow(e.f.ui, &clipboard.window);
    e.focus();
    e.f.key(PrAppKey_Left, PrAppModifierFlag_Shift | PrAppModifierFlag_Alt);
    e.f.frame();
    e.f.key('C', PrAppModifierFlag_Super);
    e.f.frame();
    CHECK(clipboard.app.text == "me");
    CHECK(e.text == "copy me");
    e.f.key('X', PrAppModifierFlag_Control);
    e.f.frame();
    CHECK(e.text == "copy ");
    e.f.key('V', PrAppModifierFlag_Super);
    e.f.frame();
    CHECK(e.text == "copy ");
    clipboard.answer();
    e.f.frame();
    CHECK(e.text == "copy me");
    // A paste that the user chose from a menu inserts at the caret of the focused control.
    e.f.push(PrAppEvent{.kind = PrAppEventKind_Paste, .input = {.text = "!"}});
    e.f.frame();
    CHECK(e.text == "copy me!");
    // A read that is pending when the context leaves the window is cancelled.
    e.f.key('V', PrAppModifierFlag_Super);
    e.f.frame();
    REQUIRE(clipboard.app.pending != nullptr);
    prUiSetWindow(e.f.ui, nullptr);
    CHECK(clipboard.app.pending == nullptr);
    CHECK(clipboard.app.cancels == 1);
}

TEST_CASE("the tree paints the selection as a highlight and the caret while the field has focus") {
    Edited e("abc");
    PrLayoutCaret caret{};
    CHECK_FALSE(prLayoutTreeCarets(e.f.tree, &caret, 1) == 1);
    e.focus();
    REQUIRE(prLayoutTreeCarets(e.f.tree, &caret, 1) == 1);
    CHECK(caret.position.node == e.textNode());
    CHECK(caret.position.offset == 3);
    e.f.key(PrAppKey_Left, PrAppModifierFlag_Shift);
    e.f.frame();
    PrLayoutHighlight highlight{};
    REQUIRE(prLayoutNodeHighlights(e.f.tree, e.textNode(), &highlight, 1) == 1);
    CHECK(highlight.start == 2);
    CHECK(highlight.end == 3);
    // A selection hides the caret.
    CHECK_FALSE(prLayoutTreeCarets(e.f.tree, &caret, 1) == 1);
    // Losing focus hides both.
    e.f.click(PrVec2{700, 500});
    e.f.frame();
    CHECK(prLayoutNodeHighlights(e.f.tree, e.textNode(), nullptr, 0) == 0);
    CHECK_FALSE(prLayoutTreeCarets(e.f.tree, &caret, 1) == 1);
}

TEST_CASE("the caret blinks while the field has focus, and holds still if the user asks for less motion") {
    Edited e("abc");
    e.focus();
    CHECK(!prLayoutTreeCaretsHidden(e.f.tree));
    CHECK(prUiOutput(e.f.ui).frameTime != 0);
    CHECK_FALSE(prUiOutput(e.f.ui).wantsFrame);
    e.f.frame(0.6);
    CHECK(prLayoutTreeCaretsHidden(e.f.tree));
    e.f.frame(0.5);
    CHECK(!prLayoutTreeCaretsHidden(e.f.tree));
    e.f.preferences.reducedMotion = true;
    e.f.frame(0.6);
    CHECK(!prLayoutTreeCaretsHidden(e.f.tree));
    CHECK(prUiOutput(e.f.ui).frameTime == 0);
}

TEST_CASE("Return inserts a line feed in a text area, and Up and Down move between its lines") {
    Edited e("one", true);
    e.focus();
    e.f.key(PrAppKey_Enter);
    e.f.frame();
    e.f.type("two");
    e.f.frame();
    CHECK(e.text == "one\ntwo");
    e.f.key(PrAppKey_Up);
    e.f.frame();
    CHECK(e.selection().caret == 3);
    e.f.key(PrAppKey_Down);
    e.f.frame();
    CHECK(e.selection().caret == 7);
    // The caret stands on the empty line after a final line feed.
    e.f.key(PrAppKey_Enter);
    e.f.frame();
    e.f.frame();
    PrLayoutRect two{};
    PrLayoutRect after{};
    REQUIRE(prLayoutNodeTextCaret(e.f.tree, e.textNode(), 4, PrLayoutTextAffinity_Downstream,
        PrLayoutCoordinates_Layout, &two));
    REQUIRE(prLayoutNodeTextCaret(e.f.tree, e.textNode(), 8, PrLayoutTextAffinity_Downstream,
        PrLayoutCoordinates_Layout, &after));
    CHECK(after.top > two.top);
    CHECK(after.left == two.left);
    e.f.key(PrAppKey_Up);
    e.f.frame();
    CHECK(e.selection().caret == 4);
    CHECK(kitVerified(e.f.tree));
}

TEST_CASE("a text area scrolls to keep the caret in view") {
    Edited e("", true, 2);
    e.focus();
    for (int i = 0; i < 6; ++i) {
        e.f.type("line");
        e.f.key(PrAppKey_Enter);
        e.f.frame();
    }
    e.f.frame();
    PrLayoutScrollInfo info{};
    REQUIRE(prLayoutNodeScrollInfo(e.f.tree, e.node, &info));
    CHECK(info.offset.y.raw > 0);
    PrLayoutRect caret{};
    REQUIRE(prLayoutNodeTextCaret(e.f.tree, e.textNode(), e.selection().caret, PrLayoutTextAffinity_Downstream,
        PrLayoutCoordinates_Viewport, &caret));
    PrRect box = e.f.bounds(e.node);
    CHECK(prLayoutUnitToPx(caret.bottom) <= box.bottom);
    CHECK(prLayoutUnitToPx(caret.top) >= box.top);
}

TEST_CASE("a text field scrolls along its line to keep the caret in view, and back with Home") {
    Edited e("");
    e.focus();
    e.f.type("a line of text much longer than the field is wide");
    e.f.frame();
    e.f.frame();
    // The value is the field's first child, and scrolls inside the field's frame.
    PrLayoutNode value = prLayoutNodeFirstChild(e.f.tree, e.node);
    PrLayoutScrollInfo info{};
    REQUIRE(prLayoutNodeScrollInfo(e.f.tree, value, &info));
    CHECK(info.offset.x.raw > 0);
    PrLayoutRect caret{};
    REQUIRE(prLayoutNodeTextCaret(e.f.tree, e.textNode(), e.selection().caret, PrLayoutTextAffinity_Downstream,
        PrLayoutCoordinates_Viewport, &caret));
    CHECK(prLayoutUnitToPx(caret.left) <= e.f.bounds(value).right);
    e.f.key(PrAppKey_Home);
    e.f.frame();
    e.f.frame();
    REQUIRE(prLayoutNodeScrollInfo(e.f.tree, value, &info));
    CHECK(info.offset.x.raw == 0);
}

TEST_CASE("a caller that keeps its old text rejects an edit") {
    Edited e("fixed");
    e.focus();
    e.f.type("x");
    e.f.frame();
    CHECK(e.text == "fixedx");
    e.text = "fixed";
    e.f.frame();
    CHECK(prLayoutNodeText(e.f.tree, e.textNode()) == "fixed");
}

TEST_CASE("text that the caller changes during an edit replaces the text being edited") {
    Edited e("abcdef");
    e.focus();
    e.f.type("g");
    e.f.frame();
    CHECK(e.text == "abcdefg");
    e.text = "xy";
    e.f.frame();
    e.f.type("z");
    e.f.frame();
    CHECK(e.text == "xyz");
}

TEST_CASE("a field over the caller's buffer edits it, and the caller's other edits carry the caret") {
    KitFixture f;
    PrTextEditBuffer* buffer = prTextEditBufferCreate("abc");
    PrTextEditHistory* history = prTextEditHistoryCreate(buffer);
    PrLayoutNode field = kPrLayoutNodeNone;
    bool reported = false;
    f.build = [&](PrLayoutNode root) {
        PrKitTextField content{.buffer = buffer};
        field = prKitTextField(f.kit, root, content, {});
        PrStringView text{};
        reported = prKitTextChanged(f.kit, field, &text) || reported;
    };
    f.frame();
    f.click(f.center(field));
    f.frame();
    prUiTextEditSetSelection(f.ui, field, PrUiTextSelection{3, 3});
    f.frame();
    f.type("d");
    f.frame();
    CHECK(prTextEditBufferText(buffer) == "abcd");
    CHECK(reported);

    PrTextEditChange before{.start = 0, .end = 0, .text = "zz"};
    prTextEditBufferApply(buffer, &before, 1, PrTextEditOrigin_Remote);
    f.frame();
    CHECK(prUiTextEditSelection(f.ui, field).caret == 6);

    // The history of the buffer belongs to its owner, and it records what the field did.
    PrTextEditChange more{.start = 6, .end = 6, .text = "e"};
    prTextEditBufferApply(buffer, &more, 1, PrTextEditOrigin_Local);
    CHECK(prTextEditHistoryCanUndo(history));
    prTextEditHistoryDestroy(history);
    f.frame();
    prTextEditBufferDestroy(buffer);
}

TEST_CASE("marked text stays inside a value's edit until it commits") {
    Edited e("ab");
    uint32_t commits = 0;
    e.f.editContext = &commits;
    e.f.edit = [](void* context, PrLayoutNode, PrUiEditPhase phase) {
        *static_cast<uint32_t*>(context) += phase == PrUiEditPhase_Commit ? 1 : 0;
    };
    e.focus();

    e.f.push(PrAppEvent{.kind = PrAppEventKind_CompositionUpdate, .input = {.text = "k"}});
    e.f.frame();
    e.f.frame();
    CHECK(e.text == "ab");
    CHECK(commits == 0);
    CHECK(prLayoutNodeText(e.f.tree, e.textNode()) == "abk");

    e.f.push(PrAppEvent{.kind = PrAppEventKind_CompositionUpdate, .input = {.text = "\xe3\x81\x8b"}});
    e.f.frame();
    e.f.frame();
    CHECK(e.text == "ab");
    CHECK(prLayoutNodeText(e.f.tree, e.textNode()) == "ab\xe3\x81\x8b");

    e.f.push(PrAppEvent{.kind = PrAppEventKind_CompositionEnd, .input = {}});
    e.f.push(PrAppEvent{.kind = PrAppEventKind_Text, .input = {.text = "\xe3\x81\x8b"}});
    e.f.frame();
    CHECK(e.text == "ab\xe3\x81\x8b");
    CHECK(commits == 1);
}

TEST_CASE("the keys that an input method takes for its composition do not edit the text") {
    Edited e("ab");
    uint32_t commits = 0;
    e.f.editContext = &commits;
    e.f.edit = [](void* context, PrLayoutNode, PrUiEditPhase phase) {
        *static_cast<uint32_t*>(context) += phase == PrUiEditPhase_Commit ? 1 : 0;
    };
    e.focus();

    e.f.push(PrAppEvent{.kind = PrAppEventKind_KeyDown, .keyboard = {.key = 'K', .composing = true, .text = "k"}});
    e.f.push(PrAppEvent{.kind = PrAppEventKind_CompositionUpdate, .input = {.text = "k"}});
    e.f.push(PrAppEvent{.kind = PrAppEventKind_KeyDown, .keyboard = {.key = PrAppKey_Backspace, .composing = true}});
    e.f.push(PrAppEvent{.kind = PrAppEventKind_CompositionUpdate, .input = {.text = "\xe3\x81\x8b"}});
    e.f.frame();
    e.f.frame();
    CHECK(prLayoutNodeText(e.f.tree, e.textNode()) == "ab\xe3\x81\x8b");

    e.f.push(PrAppEvent{.kind = PrAppEventKind_KeyDown, .keyboard = {.key = PrAppKey_Enter, .composing = true}});
    e.f.push(PrAppEvent{.kind = PrAppEventKind_CompositionEnd, .input = {.text = "\xe3\x81\x8b"}});
    e.f.push(PrAppEvent{.kind = PrAppEventKind_Text, .input = {.text = "\xe3\x81\x8b"}});
    e.f.frame();
    CHECK(e.text == "ab\xe3\x81\x8b");
    CHECK(commits == 1);
}

TEST_CASE("text that the caller changes during a composition ends the composition") {
    Edited e("ab");
    e.focus();
    e.f.push(PrAppEvent{.kind = PrAppEventKind_CompositionUpdate, .input = {.text = "k"}});
    e.f.frame();
    e.text = "xy";
    e.f.frame();
    CHECK(prLayoutNodeText(e.f.tree, e.textNode()) == "xy");
}

TEST_CASE("a value's edit keeps its selection when focus leaves, and takes the caller's text when it returns") {
    Edited e("abcd");
    e.focus();
    e.f.type("e");
    e.f.frame();
    uint32_t caret = e.selection().caret;
    prUiSetFocus(e.f.ui, kPrLayoutNodeNone);
    e.f.frame();
    e.f.frame();
    CHECK(e.selection().caret == caret);

    e.text = "xyz";
    e.f.frame();
    // A second press at the same place selects a unit, so End collapses the selection first.
    e.focus();
    e.f.key(PrAppKey_End);
    e.f.frame();
    e.f.type("!");
    e.f.frame();
    CHECK(e.text == "xyz!");
}

TEST_CASE("marked text in the caller's buffer leaves with focus") {
    KitFixture f;
    PrTextEditBuffer* buffer = prTextEditBufferCreate("ab");
    PrLayoutNode field = kPrLayoutNodeNone;
    bool reported = false;
    f.build = [&](PrLayoutNode root) {
        PrKitTextField content{.buffer = buffer};
        field = prKitTextField(f.kit, root, content, {});
        PrStringView text{};
        reported = prKitTextChanged(f.kit, field, &text) || reported;
    };
    f.frame();
    f.click(f.center(field));
    f.frame();
    f.push(PrAppEvent{.kind = PrAppEventKind_CompositionUpdate, .input = {.text = "k"}});
    f.frame();
    CHECK(prTextEditBufferSize(buffer) == 3);
    CHECK_FALSE(reported);
    prUiSetFocus(f.ui, kPrLayoutNodeNone);
    f.frame();
    f.frame();
    CHECK(prTextEditBufferText(buffer) == "ab");
    prTextEditBufferDestroy(buffer);
}

TEST_CASE("a disabled field takes no text and shows no caret") {
    KitFixture f;
    String text = "fixed";
    PrLayoutNode field = kPrLayoutNodeNone;
    f.build = [&](PrLayoutNode root) {
        field = taken(f.kit, prKitTextField(f.kit, root, {.text = text, .disabled = true}, {}), text);
    };
    f.frame();
    f.click(f.center(field));
    f.frame();
    f.type("x");
    f.frame();
    CHECK(text == "fixed");
    PrLayoutCaret caret{};
    CHECK_FALSE(prLayoutTreeCarets(f.tree, &caret, 1) == 1);
}
