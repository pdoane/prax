#include "pr/command.h"
#include "pr/command_keymap.h"
#include "pr/json.h"

#include "doctest/doctest.h"

#include <string>
#include <vector>

extern "C" int prTestCommandCApi(void);

namespace pr_impl {

namespace {

// What the commands of a test did.
struct Log {
    std::vector<std::string> runs;
    bool disabled = false;
    bool checked = false;
};

void record(void* PR_NULLABLE context, const PrCommandArgs* args) {
    Log* log = static_cast<Log*>(context);
    std::string run = "run";
    if (prCommandArgsHas(args, "name")) {
        PrStringView name = prCommandArgsString(args, "name", "");
        run += " " + std::string(name.data, name.size);
    }
    if (prCommandArgsHas(args, "count"))
        run += " " + std::to_string(int(prCommandArgsNumber(args, "count", 0)));
    if (prCommandArgsBool(args, "loud", false))
        run += "!";
    log->runs.push_back(run);
    if (args->result != nullptr)
        prJsonWriterNumber(args->result, double(log->runs.size()));
}

PrCommandStateFlags stateOf(void* PR_NULLABLE context) {
    const Log* log = static_cast<const Log*>(context);
    return PrCommandStateFlags((log->disabled ? PrCommandStateFlag_Disabled : 0) |
        (log->checked ? PrCommandStateFlag_Checked : 0));
}

const PrCommandParam kParams[] = {
    {.name = "name", .kind = PrCommandParamKind_String, .required = true, .description = "who to greet"},
    {.name = "count", .kind = PrCommandParamKind_Number},
    {.name = "loud", .kind = PrCommandParamKind_Bool},
};

std::string stringOf(PrString* string) {
    PrStringView view = prStringView(string);
    std::string out(view.data != nullptr ? view.data : "", view.size);
    prStringReset(string);
    return out;
}

// Runs `id` with the arguments that the JSON `text` holds, and returns the status and, through `error`, why it failed.
PrStatus runWith(const PrCommandRegistry* registry, const char* id, const char* text, std::string* error = nullptr,
        PrJsonWriter* result = nullptr) {
    PrJson* document = prJsonCreate();
    REQUIRE(prJsonParse(document, text, nullptr));
    PrCommandArgs args{.document = document, .object = 0, .result = result};
    PrString why = prStringNull();
    PrStatus status = prCommandRegistryRun(registry, id, &args, &why);
    std::string explained = stringOf(&why);
    if (error != nullptr)
        *error = explained;
    prJsonDestroy(document);
    return status;
}

PrCommandChord chordOf(const char* text) {
    PrCommandChord chord{};
    CHECK(prCommandChordParse(text, &chord));
    return chord;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("the public headers compile as C") {
    CHECK(prTestCommandCApi() == 0);
}

TEST_CASE("a registry keeps its commands in order, by id, with copies of what describes them") {
    Log log;
    PrCommandRegistry* registry = prCommandRegistryCreate();
    std::string id = "app.greet";
    PrCommandDesc greet{
        .id = id,
        .title = "Greet",
        .category = "App",
        .params = kParams,
        .paramCount = 3,
        .run = record,
        .context = &log,
    };
    prCommandRegistryAdd(registry, &greet);
    id = "overwritten";
    PrCommandDesc quit{
        .id = "app.quit",
        .title = "Quit",
        .flags = PrCommandFlag_Hidden,
        .run = record,
        .state = stateOf,
        .context = &log,
    };
    prCommandRegistryAdd(registry, &quit);
    REQUIRE(prCommandRegistryCount(registry) == 2);
    CHECK(prCommandRegistryFind(registry, "app.greet") == 0);
    CHECK(prCommandRegistryFind(registry, "app.quit") == 1);
    CHECK(prCommandRegistryFind(registry, "overwritten") == kPrCommandMissing);
    PrCommandDesc stored = prCommandRegistryCommand(registry, 0);
    CHECK(stored.title == "Greet");
    CHECK(stored.category == "App");
    REQUIRE(stored.paramCount == 3);
    CHECK(stored.params[0].name == "name");
    CHECK(stored.params[0].description == "who to greet");
    CHECK(!prCommandRegistryIsDirect(registry, 0));
    CHECK(prCommandRegistryIsDirect(registry, 1));
    CHECK(prCommandRegistryCommand(registry, 1).flags == PrCommandFlag_Hidden);
    CHECK(prCommandRegistryState(registry, 0) == 0);
    log.checked = true;
    CHECK(prCommandRegistryState(registry, 1) == PrCommandStateFlag_Checked);
    prCommandRegistryDestroy(registry);
}

TEST_CASE("a run checks its arguments against the parameters, and a disabled command does not run") {
    Log log;
    PrCommandRegistry* registry = prCommandRegistryCreate();
    PrCommandDesc greet{
        .id = "app.greet",
        .params = kParams,
        .paramCount = 3,
        .run = record,
        .state = stateOf,
        .context = &log,
    };
    prCommandRegistryAdd(registry, &greet);
    PrJsonWriter* result = prJsonWriterCreate();
    CHECK(runWith(registry, "app.greet", "{\"name\": \"ada\", \"count\": 2, \"loud\": true}", nullptr, result) ==
        PrStatus_Ok);
    CHECK(prJsonWriterText(result) == "1");
    CHECK(runWith(registry, "app.greet", "{\"name\": \"bo\"}") == PrStatus_Ok);
    CHECK(log.runs == std::vector<std::string>{"run ada 2!", "run bo"});

    std::string error;
    CHECK(runWith(registry, "app.greet", "{}", &error) == PrStatus_InvalidData);
    CHECK(error == "The command app.greet needs the parameter \"name\".");
    CHECK(runWith(registry, "app.greet", "{\"name\": 1}", &error) == PrStatus_InvalidData);
    CHECK(error == "The command app.greet was given the wrong kind of value for \"name\", which takes a string.");
    CHECK(runWith(registry, "app.greet", "{\"name\": \"x\", \"color\": 1}", &error) == PrStatus_InvalidData);
    CHECK(error == "The command app.greet has no parameter \"color\".");
    CHECK(runWith(registry, "app.greet", "[1]", &error) == PrStatus_InvalidData);
    CHECK(runWith(registry, "app.missing", "{}", &error) == PrStatus_NotFound);
    CHECK(error == "The command app.missing does not exist.");
    CHECK(prCommandRegistryRun(registry, "app.greet", nullptr, nullptr) == PrStatus_InvalidData);
    log.disabled = true;
    CHECK(runWith(registry, "app.greet", "{\"name\": \"ada\"}", &error) == PrStatus_InvalidState);
    CHECK(error == "The command app.greet is disabled.");
    CHECK(log.runs.size() == 2);
    prJsonWriterDestroy(result);
    prCommandRegistryDestroy(registry);
}

TEST_CASE("a chord reads from modifiers and a key, and shows as the platform shows shortcuts") {
    CHECK(chordOf("Cmd Shift P").key == 'P');
    PrAppModifierFlags primary = kPrCommandPrimaryModifier;
    CHECK(chordOf("Cmd Shift P").modifiers == (primary | PrAppModifierFlag_Shift));
    CHECK(chordOf("cmd p").key == 'P');
    CHECK(chordOf("Cmd =").key == '=');
    CHECK(chordOf("Ctrl Alt Enter").key == PrAppKey_Enter);
    CHECK(chordOf("Ctrl Alt Enter").modifiers == (PrAppModifierFlag_Control | PrAppModifierFlag_Alt));
    CHECK(chordOf("Opt Up").key == PrAppKey_Up);
    CHECK(chordOf("ArrowLeft").key == PrAppKey_Left);
    CHECK(chordOf("F5").key == PrAppKey_F5);
    CHECK(chordOf("Space").key == ' ');
    CHECK(chordOf("I").modifiers == 0);
    PrCommandChord chord{};
    for (const char* bad : {"", "Cmd", "Cmd Shift", "P Cmd", "Hyper P", "Cmd PP"})
        CHECK_FALSE(prCommandChordParse(bad, &chord));

    PrString text = prCommandChordText(chordOf("Cmd Shift P"));
#if defined(__APPLE__)
    CHECK(stringOf(&text) == "\xE2\x87\xA7\xE2\x8C\x98P");
#else
    CHECK(stringOf(&text) == "Ctrl+Shift+P");
#endif
    text = prCommandChordText(chordOf("Backspace"));
#if defined(__APPLE__)
    CHECK(stringOf(&text) == "\xE2\x8C\xAB");
#else
    CHECK(stringOf(&text) == "Backspace");
#endif
}

TEST_CASE("a keymap binds a chord once per context, and a later binding replaces an earlier one") {
    PrCommandKeymap* keymap = prCommandKeymapCreate();
    prCommandKeymapBind(keymap, chordOf("Cmd Enter"), "app.run", "", PrCommandBindingKind_Normal);
    prCommandKeymapBind(keymap, chordOf("Cmd Enter"), "panel.run", "panel", PrCommandBindingKind_Normal);
    prCommandKeymapBind(keymap, chordOf("Cmd Enter"), "app.other", "", PrCommandBindingKind_Override);
    prCommandKeymapBind(keymap, chordOf("Cmd R"), "panel.run", "", PrCommandBindingKind_Normal);
    REQUIRE(prCommandKeymapCount(keymap) == 3);
    PrCommandBinding first = prCommandKeymapBinding(keymap, 0);
    CHECK(first.commandId == "app.other");
    CHECK(first.kind == PrCommandBindingKind_Override);
    CHECK(prCommandKeymapBinding(keymap, 1).context == "panel");
    PrCommandChord chord{};
    REQUIRE(prCommandKeymapChord(keymap, "panel.run", &chord));
    CHECK(chord.key == PrAppKey_Enter);
    CHECK_FALSE(prCommandKeymapChord(keymap, "app.run", &chord));
    prCommandKeymapDestroy(keymap);
}
