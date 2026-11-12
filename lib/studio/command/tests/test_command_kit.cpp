#include "pr/alloc.h"
#include "pr/array.h"
#include "pr/command.h"
#include "pr/command_keymap.h"
#include "pr/command_kit.h"
#include "pr/construct.h"
#include "pr/kit_box.h"
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
        for (Entry* entry : entries) {
            destroy(entry);
            prMemoryFree(entry);
        }
    }

    Commands() = default;
    Commands(const Commands&) = delete;
    Commands& operator=(const Commands&) = delete;

    void add(const char* id, const char* title, const char* category, PrCommandFlags flags = 0,
            const PrCommandParam* params = nullptr, uint32_t paramCount = 0) {
        entries.push(construct<Entry>(alloc<Entry>(), Entry{this, id}));
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

TEST_CASE("a scope runs the command of a chord while focus is within it, and the innermost scope wins") {
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
    // With focus in the panel, the panel's scope takes it, and a chord that only the root binds reaches the root.
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

TEST_CASE("an override takes a chord that a focused text field claims, and a normal binding does not") {
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

TEST_CASE("a disabled command's chord is taken and does nothing, and a command with a required parameter is unbound") {
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

TEST_CASE("a command's menu item shows its title, chord and check, and runs it") {
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
        item = prCommandMenuItem(f.kit, menu, c.registry, c.keymap, "view.grid", {});
        missing = prCommandMenuItem(f.kit, menu, c.registry, c.keymap, "view.none", {});
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

TEST_CASE("the command palette lists the commands that match its query, and runs the one activated") {
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
        list = prCommandPalette(f.kit, trigger, c.registry, c.keymap, query);
        if (list != kPrLayoutNodeNone)
            taken(f.kit, list, query);
    };
    f.frame();
    prKitSetOpen(f.kit, trigger, true);
    f.frame();
    f.frame();
    REQUIRE(list != kPrLayoutNodeNone);
    // Every command that a surface runs and the palette does not hide.
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
