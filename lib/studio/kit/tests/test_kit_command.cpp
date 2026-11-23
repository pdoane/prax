#include "pr/array.h"
#include "pr/command.h"
#include "pr/command_keymap.h"
#include "pr/command_ui.h"
#include "pr/construct.h"
#include "pr/kit_box.h"
#include "pr/kit_command.h"
#include "pr/kit_input.h"
#include "pr/kit_menu.h"
#include "pr/memory.h"
#include "pr/string.h"

#include "tools/harness/kit_fixture.h"

#include "tools/harness/test.h"

using namespace harness;

namespace pr_impl {

namespace {

// The commands of a test, each of which records its id when it runs.
struct Commands {
    // A command's context: its id, and the commands that it belongs to.
    struct Entry {
        Commands* commands;
        String id;
    };

    PrCommandRegistry* registry = prCommandRegistryCreate();
    PrCommandKeymap* keymap = prCommandKeymapCreate();
    Array<Entry*> entries;
    Array<String> runs;
    bool disabled = false;
    bool checked = false;

    ~Commands() {
        prCommandKeymapDestroy(keymap);
        prCommandRegistryDestroy(registry);
        for (Entry* entry : entries)
            destroy(entry);
    }

    Commands() = default;
    Commands(const Commands&) = delete;
    Commands& operator=(const Commands&) = delete;

    void add(const char* id, const char* title, const char* category, PrCommandFlags flags = 0,
            const PrCommandParam* params = nullptr, uint32_t paramCount = 0) {
        entries.push(create<Entry>(this, id));
        PrCommandDesc desc{
            .id = id,
            .title = title,
            .category = category,
            .flags = flags,
            .params = params,
            .paramCount = paramCount,
            .run = run,
            .state = state,
            .context = entries.last(),
        };
        prCommandRegistryAdd(registry, &desc);
    }

    void bind(const char* chord, const char* id, const char* context = "",
            PrCommandBindingKind kind = PrCommandBindingKind_Normal) {
        PrCommandChord parsed{};
        REQUIRE(prCommandChordParse(chord, &parsed));
        prCommandKeymapBind(keymap, parsed, id, context, kind);
    }

    static void run(void* PR_NULLABLE context, const PrCommandArgs*) {
        Entry* entry = static_cast<Entry*>(context);
        entry->commands->runs.push(entry->id);
    }

    static PrCommandStateFlags state(void* PR_NULLABLE context) {
        const Commands* commands = static_cast<const Entry*>(context)->commands;
        return PrCommandStateFlags((commands->disabled ? PrCommandStateFlag_Disabled : 0) |
            (commands->checked ? PrCommandStateFlag_Checked : 0));
    }
};

// Writes the text of the control `field` to `owned`, if it changed in the round, and returns the control.
PrLayoutNode taken(PrKit* kit, PrLayoutNode field, String& owned) {
    PrStringView text{};
    if (prKitTextChanged(kit, field, &text))
        owned.assign(text);
    return field;
}

uint32_t childCount(const PrLayoutTree* tree, PrLayoutNode node) {
    uint32_t count = 0;
    for (PrLayoutNode child = prLayoutNodeFirstChild(tree, node); child != kPrLayoutNodeNone;
            child = prLayoutNodeNextSibling(tree, child))
        ++count;
    return count;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("a scope runs the command bound to a chord while focus is within the scope, and the innermost scope wins") {
    KitFixture f;
    Commands c;
    c.add("app.save", "Save", "File");
    c.add("panel.save", "Save Panel", "Panel");
    c.bind("Cmd S", "app.save");
    c.bind("Cmd S", "panel.save", "panel");
    c.bind("Cmd O", "app.save");
    PrLayoutNode panel = kPrLayoutNodeNone;
    PrLayoutNode field = kPrLayoutNodeNone;
    String text = "";
    f.build = [&](PrLayoutNode root) {
        prCommandScope(f.ui, root, c.registry, c.keymap, "");
        panel = prKitBox(f.kit, root, {}, {});
        prCommandScope(f.ui, panel, c.registry, c.keymap, "panel");
        field = taken(f.kit, prKitTextField(f.kit, panel, {.text = text}, {}), text);
    };
    f.frame();
    // With nothing focused, the root's scope takes the chord.
    f.key('S', kPrCommandPrimaryModifier);
    f.frame();
    CHECK(c.runs == Array<String>({"app.save"}));
    // With focus in the panel, the panel's scope takes the chord. A chord that only the root binds reaches the root.
    f.click(f.center(field));
    f.frame();
    REQUIRE(prUiIsFocusWithin(f.ui, panel));
    c.runs.clear();
    f.key('S', kPrCommandPrimaryModifier);
    f.frame();
    f.key('O', kPrCommandPrimaryModifier);
    f.frame();
    CHECK(c.runs == Array<String>({"panel.save", "app.save"}));
}

TEST_CASE("an override binding runs on a chord that a focused text field claims, and a normal binding does not") {
    KitFixture f;
    Commands c;
    c.add("image.delete", "Delete Image", "Image");
    c.add("image.left", "Previous Image", "Image");
    c.bind("Cmd Backspace", "image.delete", "", PrCommandBindingKind_Override);
    c.bind("Left", "image.left");
    PrLayoutNode field = kPrLayoutNodeNone;
    String text = "abc";
    f.build = [&](PrLayoutNode root) {
        prCommandScope(f.ui, root, c.registry, c.keymap, "");
        field = taken(f.kit, prKitTextField(f.kit, root, {.text = text}, {}), text);
    };
    f.frame();
    f.click(f.center(field));
    f.frame();
    f.key(PrAppKey_Left);
    f.frame();
    CHECK(c.runs.isEmpty());
    f.key(PrAppKey_Backspace, kPrCommandPrimaryModifier);
    f.frame();
    CHECK(c.runs == Array<String>({"image.delete"}));
    CHECK(text == "abc");
}

TEST_CASE("the chord of a disabled command runs nothing, and a chord runs no command that has a required parameter") {
    KitFixture f;
    Commands c;
    const PrCommandParam required[] = {{.name = "name", .kind = PrCommandParamKind_String, .required = true}};
    c.add("app.run", "Run", "");
    c.add("app.named", "Named", "", 0, required, 1);
    c.bind("Cmd R", "app.run");
    c.bind("Cmd N", "app.named");
    f.build = [&](PrLayoutNode root) { prCommandScope(f.ui, root, c.registry, c.keymap, ""); };
    f.frame();
    c.disabled = true;
    f.key('R', kPrCommandPrimaryModifier);
    f.frame();
    CHECK(c.runs.isEmpty());
    c.disabled = false;
    f.key('R', kPrCommandPrimaryModifier);
    f.frame();
    CHECK(c.runs.count() == 1);
    f.key('N', kPrCommandPrimaryModifier);
    f.frame();
    CHECK(c.runs.count() == 1);
}

TEST_CASE("a press on the empty space of a scope focuses the scope, so its chords apply without a focusable control") {
    KitFixture f;
    Commands c;
    c.add("app.help", "Help", "");
    c.add("panel.next", "Next", "");
    c.bind("F1", "app.help");
    c.bind("Right", "panel.next", "panel");
    PrLayoutNode panel = kPrLayoutNodeNone;
    PrLayoutNode inner = kPrLayoutNodeNone;
    f.build = [&](PrLayoutNode root) {
        // A panel below the root, padded around a box that takes nothing. A key starts at the root while nothing is
        // focused.
        panel = prKitBox(f.kit, root, {}, {.padding = {20, 20, 20, 20}});
        prCommandScope(f.ui, panel, c.registry, c.keymap, "");
        inner = prKitBox(f.kit, panel, {}, {.padding = {20, 20, 20, 20}});
        prCommandScope(f.ui, inner, c.registry, c.keymap, "panel");
    };
    f.frame();
    // With nothing focused, a key starts at the root, above both scopes.
    f.key(PrAppKey_F1);
    f.frame();
    CHECK(c.runs.isEmpty());
    // A press in the inner box's space focuses it, and keys then reach both scopes.
    f.click(f.center(inner));
    f.frame();
    CHECK(prUiFocus(f.ui) == inner);
    f.key(PrAppKey_Right);
    f.frame();
    f.key(PrAppKey_F1);
    f.frame();
    CHECK(c.runs == Array<String>({"panel.next", "app.help"}));
}

TEST_CASE("a held chord runs its command on each key repeat, unless the command is flagged to run once per press") {
    KitFixture f;
    Commands c;
    c.add("view.next", "Next", "");
    c.add("file.delete", "Delete", "", PrCommandFlag_Once);
    c.bind("Right", "view.next");
    c.bind("Delete", "file.delete");
    f.build = [&](PrLayoutNode root) { prCommandScope(f.ui, root, c.registry, c.keymap, ""); };
    f.frame();
    const PrAppKey keys[] = {PrAppKey_Right, PrAppKey_Delete};
    for (PrAppKey key : keys) {
        f.push(PrAppEvent{.kind = PrAppEventKind_KeyDown, .keyboard = {.key = key}});
        f.push(PrAppEvent{.kind = PrAppEventKind_KeyDown, .keyboard = {.key = key, .repeat = true}});
        f.push(PrAppEvent{.kind = PrAppEventKind_KeyDown, .keyboard = {.key = key, .repeat = true}});
        f.push(PrAppEvent{.kind = PrAppEventKind_KeyUp, .keyboard = {.key = key}});
        f.frame();
    }
    CHECK(c.runs == Array<String>({"view.next", "view.next", "view.next", "file.delete"}));
}

TEST_CASE("the menu item of a command shows the command's title, chord and check, and a click runs the command") {
    KitFixture f;
    Commands c;
    c.add("view.grid", "Show Grid", "View");
    c.bind("Cmd G", "view.grid");
    c.checked = true;
    PrLayoutNode trigger = kPrLayoutNodeNone;
    PrLayoutNode item = kPrLayoutNodeNone;
    PrLayoutNode missing = kPrLayoutNodeNone;
    f.build = [&](PrLayoutNode root) {
        trigger = root;
        item = kPrLayoutNodeNone;
        PrLayoutNode popover = prKitPopover(f.kit, trigger, {}, {});
        if (popover == kPrLayoutNodeNone)
            return;
        PrLayoutNode menu = prKitMenu(f.kit, popover, {}, {});
        item = prKitRegistryMenuItem(f.kit, menu, c.registry, c.keymap, "view.grid", {});
        missing = prKitRegistryMenuItem(f.kit, menu, c.registry, c.keymap, "view.none", {});
    };
    f.frame();
    prKitSetOpen(f.kit, trigger, true);
    f.frame();
    f.frame();
    REQUIRE(item != kPrLayoutNodeNone);
    CHECK(missing == kPrLayoutNodeNone);
    f.click(f.center(item));
    f.frame();
    CHECK(c.runs.count() == 1);
    CHECK_FALSE(prKitIsOpen(f.kit, trigger));
}

TEST_CASE("the button of a command runs the command, and the button of a disabled command does nothing") {
    KitFixture f;
    Commands c;
    c.add("go.back", "Back", "Go");
    c.bind("Cmd [", "go.back");
    PrLayoutNode button = kPrLayoutNodeNone;
    PrLayoutNode missing = kPrLayoutNodeNone;
    f.build = [&](PrLayoutNode root) {
        button = prKitRegistryButton(f.kit, root, c.registry, c.keymap, "go.back", {.icon = PrStudioIconKind_ArrowLeft}, {});
        missing =
            prKitRegistryButton(f.kit, root, c.registry, c.keymap, "go.none", {.icon = PrStudioIconKind_ArrowLeft}, {});
    };
    f.frame();
    REQUIRE(button != kPrLayoutNodeNone);
    CHECK(missing == kPrLayoutNodeNone);
    f.click(f.center(button));
    f.frame();
    CHECK(c.runs == Array<String>({"go.back"}));
    c.disabled = true;
    f.frame();
    f.click(f.center(button));
    f.frame();
    CHECK(c.runs.count() == 1);
}

TEST_CASE("the button of a command shows the given text, icon and style, the same as a Kit button") {
    KitFixture f;
    Commands c;
    c.add("view.zoomReset", "Actual Size", "View");
    c.add("go.back", "Back", "Go");
    PrKitButtonStyle small{.variant = PrKitButtonVariant_Ghost, .size = PrKitControlSize_Sm};
    PrKitButtonStyle large{.size = PrKitControlSize_Lg};
    PrLayoutNode text = kPrLayoutNodeNone;
    PrLayoutNode textShown = kPrLayoutNodeNone;
    PrLayoutNode both = kPrLayoutNodeNone;
    PrLayoutNode bothShown = kPrLayoutNodeNone;
    PrLayoutNode icon = kPrLayoutNodeNone;
    f.build = [&](PrLayoutNode root) {
        PrLayoutNode row = prKitBox(f.kit, root, {}, {.direction = PrKitDirection_Row, .align = PrKitAlign_Start});
        text = prKitRegistryButton(f.kit, row, c.registry, c.keymap, "view.zoomReset", {.text = "150%"}, small);
        textShown = prKitButton(f.kit, row, {.text = "150%"}, small);
        PrKitButton back{.text = "Back", .icon = PrStudioIconKind_ArrowLeft};
        both = prKitRegistryButton(f.kit, row, c.registry, c.keymap, "go.back", back, large);
        bothShown = prKitButton(f.kit, row, back, large);
        icon = prKitRegistryButton(f.kit, row, c.registry, c.keymap, "go.back", {.icon = PrStudioIconKind_ArrowLeft}, small);
    };
    f.frame();
    auto size = [&](PrLayoutNode node) {
        PrRect box = f.bounds(node);
        return PrVec2{box.right - box.left, box.bottom - box.top};
    };
    CHECK(size(text).x == size(textShown).x);
    CHECK(size(text).y == size(textShown).y);
    CHECK(size(both).x == size(bothShown).x);
    CHECK(size(both).y == size(bothShown).y);
    // A small button of text is wider than the square of a small icon, and a large button is taller than both.
    CHECK(size(text).x > size(icon).x);
    CHECK(size(text).y == size(icon).y);
    CHECK(size(both).y > size(text).y);
    f.click(f.center(text));
    f.frame();
    f.click(f.center(both));
    f.frame();
    CHECK(c.runs == Array<String>({"view.zoomReset", "go.back"}));
}

TEST_CASE("the button of a command shows the command's checked state as its selection, not the given button's "
        "selection") {
    KitFixture f;
    Commands c;
    c.add("view.grid", "Show Grid", "View");
    c.checked = true;
    Array<uint8_t> checked;
    Array<uint8_t> unchecked;
    f.build = [&](PrLayoutNode root) {
        PrKitButton grid{.text = "Grid", .selected = !c.checked};
        prKitRegistryButton(f.kit, root, c.registry, c.keymap, "view.grid", grid, {});
    };
    f.frame();
    REQUIRE(f.render(&checked));
    c.checked = false;
    f.frame();
    REQUIRE(f.render(&unchecked));
    // The button that a Kit draws for the same content and selection.
    Array<uint8_t> shown;
    f.build = [&](PrLayoutNode root) { prKitButton(f.kit, root, {.text = "Grid", .selected = true}, {}); };
    f.frame();
    REQUIRE(f.render(&shown));
    CHECK(checked == shown);
    CHECK(unchecked != shown);
}

TEST_CASE("the command palette lists the commands that match its query, and runs the command that is activated") {
    KitFixture f;
    Commands c;
    const PrCommandParam required[] = {{.name = "name", .kind = PrCommandParamKind_String, .required = true}};
    c.add("file.open", "Open File", "File");
    c.add("file.save", "Save", "File");
    c.add("view.zoom", "Zoom In", "View");
    c.add("app.hidden", "Hidden Open", "File", PrCommandFlag_Hidden);
    c.add("app.named", "Open Named", "File", 0, required, 1);
    String query = "";
    PrLayoutNode trigger = kPrLayoutNodeNone;
    PrLayoutNode list = kPrLayoutNodeNone;
    f.build = [&](PrLayoutNode root) {
        trigger = root;
        list = prKitRegistryPalette(f.kit, trigger, c.registry, c.keymap, query);
        if (list != kPrLayoutNodeNone)
            taken(f.kit, list, query);
    };
    f.frame();
    prKitSetOpen(f.kit, trigger, true);
    f.frame();
    f.frame();
    REQUIRE(list != kPrLayoutNodeNone);
    // The list has every command that an entry point can run and that the palette does not hide.
    CHECK(childCount(f.tree, list) == 3);
    f.type("file");
    f.frame();
    f.frame();
    REQUIRE(list != kPrLayoutNodeNone);
    CHECK(childCount(f.tree, list) == 2);
    f.key(PrAppKey_Down);
    f.frame();
    f.key(PrAppKey_Enter);
    f.frame();
    CHECK(c.runs == Array<String>({"file.save"}));
    f.frame();
    CHECK(list == kPrLayoutNodeNone);
}

TEST_CASE("a query matches if each of its words is in the title or the category") {
    CHECK(prCommandMatches("", "Open File", "File"));
    CHECK(prCommandMatches("  open  ", "Open File", ""));
    CHECK(prCommandMatches("file op", "Open", "File"));
    CHECK_FALSE(prCommandMatches("open save", "Open File", "File"));
}
