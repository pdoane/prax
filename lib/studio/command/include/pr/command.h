// Commands: an application's verbs, each named by an id, which its menus, its command palette, its key bindings and its
// automation run alike.
//
// A command's id is its identity: a keymap binds chords to ids, and every entry point that runs a command looks it up
// by id, so a verb behaves the same whichever entry point reached it. A command declares the arguments that it takes,
// which a caller passes as a JSON object and the registry checks before the command runs. A command may write a result
// as JSON for a caller that asks for one.
//
// A registry is used from one thread at a time. Commands run on the thread that runs them.

#pragma once

#include "pr/base.h"
#include "pr/json.h"
#include "pr/raw_string.h"
#include "pr/status.h"
#include "pr/string_view.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// The commands of an application, looked up by id.
typedef struct PrCommandRegistry PrCommandRegistry;

// A command of a registry, by its position in the order of registration.
typedef uint32_t PrCommandIndex;

// The index that a lookup returns when no command has the id.
enum { kPrCommandMissing = 0x7fffffff };

// The JSON kinds that an argument takes.
enum {
    PrCommandParamKind_Bool,
    PrCommandParamKind_Number,
    PrCommandParamKind_String,
};
typedef uint8_t PrCommandParamKind;

// A parameter of a command: the name of a member of the arguments object, the kind of its value, and whether every call
// must pass it. The description is written for a caller that chooses the arguments, such as an automation client.
typedef struct PrCommandParam {
    PrStringView name;
    PrCommandParamKind kind;
    bool required;
    PrStringView description;
} PrCommandParam;

// What a command is, apart from its arguments.
enum {
    PrCommandFlag_Hidden = 1u << 0,     // left out of the command palette; key bindings and callers still run it
    PrCommandFlag_Query = 1u << 1,      // reads and reports, and changes nothing
};
typedef uint8_t PrCommandFlags;

// The state of a command at the moment that an entry point asks.
enum {
    PrCommandStateFlag_Disabled = 1u << 0,  // the command cannot run; entry points show it faint
    PrCommandStateFlag_Checked = 1u << 1,   // the command toggles a setting that is on; menus show a check
};
typedef uint8_t PrCommandStateFlags;

// The arguments of one run, and where its result goes. `document` holds the arguments as the members of the object at
// `object`, or is null for a run with no arguments. `result` receives the command's result as one JSON value, or is
// null if the caller wants none.
typedef struct PrCommandArgs {
    const PrJson* PR_NULLABLE document;
    PrJsonValueIndex object;
    PrJsonWriter* PR_NULLABLE result;
} PrCommandArgs;

// Runs a command with `args`, which the registry checked against the command's parameters.
typedef void (*PrCommandRunFn)(void* PR_NULLABLE context, const PrCommandArgs* args);

// Returns the command's state. A command without a state function is enabled and unchecked.
typedef PrCommandStateFlags (*PrCommandStateFn)(void* PR_NULLABLE context);

// A command. The registry copies the strings and the parameters.
typedef struct PrCommandDesc {
    PrStringView id;            // unique in the registry, such as "app.quit"
    PrStringView title;         // what a menu or the palette shows, such as "Quit"
    PrStringView category;      // the group that the palette shows beside the title, or empty

    // What the command does beyond what its title says, for a caller that cannot see the application, or empty.
    PrStringView description;

    PrCommandFlags flags;
    const PrCommandParam* PR_NULLABLE params;
    uint32_t paramCount;

    // The command's functions, which receive `context`. `state` may be null.
    PrCommandRunFn run;
    PrCommandStateFn PR_NULLABLE state;
    void* PR_NULLABLE context;
} PrCommandDesc;

// Creates an empty registry, or frees one.
PrCommandRegistry* prCommandRegistryCreate(void);
void prCommandRegistryDestroy(PrCommandRegistry* PR_NULLABLE registry);

// Adds the command that `desc` describes, after the commands added before it. The id must not name a command of the
// registry already, and each parameter's name must be unique among the command's parameters.
void prCommandRegistryAdd(PrCommandRegistry* registry, const PrCommandDesc* desc);

// Return the number of commands, and the command at `index` as it was added. The views of the command are valid while
// the registry lives.
uint32_t prCommandRegistryCount(const PrCommandRegistry* registry);
PrCommandDesc prCommandRegistryCommand(const PrCommandRegistry* registry, PrCommandIndex index);

// Returns the index of the command whose id is `id`, or kPrCommandMissing.
PrCommandIndex prCommandRegistryFind(const PrCommandRegistry* registry, PrStringView id);

// Returns the state of the command at `index`, which its state function reports.
PrCommandStateFlags prCommandRegistryState(const PrCommandRegistry* registry, PrCommandIndex index);

// Returns true if an entry point that passes no arguments, such as a menu, a key binding or the palette, can run the
// command at `index`: it takes no required parameter.
bool prCommandRegistryIsDirect(const PrCommandRegistry* registry, PrCommandIndex index);

// Runs the command whose id is `id` with `args`, or with no arguments if `args` is null. Returns NotFound if no command
// has the id, InvalidState if the command is disabled, and InvalidData if the arguments are not an object, lack a
// required parameter, or have a member that is not a parameter or whose value is not of the parameter's kind. On
// failure, the command does not run, and `error`, if not null, receives a sentence that says why, which the caller
// releases with prRawStringReset.
PrStatus prCommandRegistryRun(const PrCommandRegistry* registry, PrStringView id, const PrCommandArgs* PR_NULLABLE args,
    PrRawString* PR_NULLABLE error);

// Return the argument named `name` of a run, or `fallback` if the run has no such argument. The registry checked each
// argument's kind before the run. A string is valid while the arguments' document is unchanged.
bool prCommandArgsHas(const PrCommandArgs* args, PrStringView name);
bool prCommandArgsBool(const PrCommandArgs* args, PrStringView name, bool fallback);
double prCommandArgsNumber(const PrCommandArgs* args, PrStringView name, double fallback);
PrStringView prCommandArgsString(const PrCommandArgs* args, PrStringView name, PrStringView fallback);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
