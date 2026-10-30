// Commands: an application's verbs, each named by an id.

#include "pr/command.h"

#include "pr/alloc.h"
#include "pr/array.h"
#include "pr/assert.h"
#include "pr/text_buffer.h"

namespace pr_impl {

namespace {

// A command, with copies of its views. Each command is allocated alone, so that the views of its strings, which may
// hold their bytes inline, stay valid as the registry grows.
struct Command {
    PrString id;
    PrString title;
    PrString category;
    PrString description;
    PrCommandFlags flags;
    PrCommandRunFn run;
    PrCommandStateFn PR_NULLABLE state;
    void* PR_NULLABLE context;

    // The parameters, as views of `paramText`, which holds each parameter's name and then its description.
    uint32_t paramCount;
    PrCommandParam* PR_NULLABLE params;
    PrString* PR_NULLABLE paramText;
};

struct CommandRegistry {
    Array<Command*> commands;   // by PrCommandIndex
};

PrStringView viewOf(const PrString& string) {
    return prStringView(&string);
}

const Command& commandAt(const CommandRegistry* registry, PrCommandIndex index) {
    PR_ASSERT(index < registry->commands.count);
    return *registry->commands.data[index];
}

PrStringView kindName(PrCommandParamKind kind) {
    switch (kind) {
        case PrCommandParamKind_Bool: return "a boolean";
        case PrCommandParamKind_Number: return "a number";
        default: return "a string";
    }
}

bool hasKind(PrJsonKind kind, PrCommandParamKind param) {
    switch (param) {
        case PrCommandParamKind_Bool: return kind == PrJsonKind_True || kind == PrJsonKind_False;
        case PrCommandParamKind_Number: return kind == PrJsonKind_Number;
        default: return kind == PrJsonKind_String;
    }
}

// Writes to `error`, if it is not null, the sentence "The command <id> <problem> "<name>"<detail>.", leaving out the
// name if it is null.
void explain(PrString* PR_NULLABLE error, PrStringView id, PrStringView problem, PrStringView name = {},
        PrStringView detail = {}) {
    if (error == nullptr)
        return;
    TextBuffer text{};
    textAppend(&text, "The command ");
    textAppend(&text, id);
    textAppendChar(&text, ' ');
    textAppend(&text, problem);
    if (name.data != nullptr) {
        textAppend(&text, " \"");
        textAppend(&text, name);
        textAppendChar(&text, '"');
    }
    textAppend(&text, detail);
    textAppendChar(&text, '.');
    prStringReset(error);
    *error = textCopy(&text);
    textReset(&text);
}

// Returns the member named `name` of the arguments, or kPrJsonMissing.
PrJsonValueIndex argument(const PrCommandArgs* args, PrStringView name) {
    if (args->document == nullptr)
        return kPrJsonMissing;
    return prJsonMember(args->document, args->object, name);
}

// Returns the parameter of `command` named `name`, or null.
const PrCommandParam* PR_NULLABLE paramOf(const Command& command, PrStringView name) {
    for (uint32_t i = 0; i < command.paramCount; ++i) {
        if (command.params[i].name == name)
            return &command.params[i];
    }
    return nullptr;
}

// Checks the arguments of a run against the parameters of `command`, and explains the first mismatch.
PrStatus checkArgs(const Command& command, const PrCommandArgs* args, PrString* PR_NULLABLE error) {
    PrStringView id = viewOf(command.id);
    const PrJson* document = args->document;
    if (document != nullptr) {
        if (prJsonKind(document, args->object) != PrJsonKind_Object) {
            explain(error, id, "takes its arguments as an object");
            return PrStatus_InvalidData;
        }
        for (PrJsonValueIndex i = prJsonFirst(document, args->object); i != kPrJsonMissing;
                i = prJsonAfter(document, args->object, i)) {
            PrStringView name = prJsonString(document, i);
            const PrCommandParam* param = paramOf(command, name);
            if (param == nullptr) {
                explain(error, id, "has no parameter", name);
                return PrStatus_InvalidData;
            }
            if (!hasKind(prJsonKind(document, i + 1), param->kind)) {
                TextBuffer detail{};
                textAppend(&detail, ", which takes ");
                textAppend(&detail, kindName(param->kind));
                explain(error, id, "was given the wrong kind of value for", name, textView(&detail));
                textReset(&detail);
                return PrStatus_InvalidData;
            }
        }
    }
    for (uint32_t i = 0; i < command.paramCount; ++i) {
        const PrCommandParam& param = command.params[i];
        if (param.required && argument(args, param.name) == kPrJsonMissing) {
            explain(error, id, "needs the parameter", param.name);
            return PrStatus_InvalidData;
        }
    }
    return PrStatus_Ok;
}

void freeCommand(Command* command) {
    prStringReset(&command->id);
    prStringReset(&command->title);
    prStringReset(&command->category);
    prStringReset(&command->description);
    for (uint32_t i = 0; i < 2 * command->paramCount; ++i)
        prStringReset(&command->paramText[i]);
    prMemoryFree(command->paramText);
    prMemoryFree(command->params);
    prMemoryFree(command);
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

// C opaque types
struct PrCommandRegistry : CommandRegistry {};

PrCommandRegistry* prCommandRegistryCreate(void) {
    return allocZeroed<PrCommandRegistry>();
}

void prCommandRegistryDestroy(PrCommandRegistry* PR_NULLABLE registry) {
    if (registry == nullptr)
        return;
    for (uint32_t i = 0; i < registry->commands.count; ++i)
        freeCommand(registry->commands.data[i]);
    registry->commands.reset();
    prMemoryFree(registry);
}

void prCommandRegistryAdd(PrCommandRegistry* registry, const PrCommandDesc* desc) {
    PR_ASSERT(desc->id.size != 0 && prCommandRegistryFind(registry, desc->id) == kPrCommandMissing);
    PR_ASSERT(desc->params != nullptr || desc->paramCount == 0);
    PR_ASSERT(desc->run != nullptr);
    Command* command = allocZeroed<Command>();
    command->id = prStringCopy(desc->id);
    command->title = prStringCopy(desc->title);
    command->category = prStringCopy(desc->category);
    command->description = prStringCopy(desc->description);
    command->flags = desc->flags;
    command->run = desc->run;
    command->state = desc->state;
    command->context = desc->context;
    command->paramCount = desc->paramCount;
    if (desc->paramCount != 0) {
        command->paramText = static_cast<PrString*>(prMemoryAllocZeroed(2 * desc->paramCount * sizeof(PrString)));
        command->params = static_cast<PrCommandParam*>(prMemoryAlloc(desc->paramCount * sizeof(PrCommandParam)));
    }
    for (uint32_t i = 0; i < desc->paramCount; ++i) {
        const PrCommandParam& param = desc->params[i];
        for (uint32_t k = 0; k < i; ++k)
            PR_ASSERT(desc->params[k].name != param.name);
        command->paramText[2 * i] = prStringCopy(param.name);
        command->paramText[2 * i + 1] = prStringCopy(param.description);
        command->params[i] = PrCommandParam{
            .name = viewOf(command->paramText[2 * i]),
            .kind = param.kind,
            .required = param.required,
            .description = viewOf(command->paramText[2 * i + 1]),
        };
    }
    registry->commands.push(command);
}

uint32_t prCommandRegistryCount(const PrCommandRegistry* registry) {
    return registry->commands.count;
}

PrCommandDesc prCommandRegistryCommand(const PrCommandRegistry* registry, PrCommandIndex index) {
    const Command& command = commandAt(registry, index);
    return PrCommandDesc{
        .id = viewOf(command.id),
        .title = viewOf(command.title),
        .category = viewOf(command.category),
        .description = viewOf(command.description),
        .flags = command.flags,
        .params = command.params,
        .paramCount = command.paramCount,
        .run = command.run,
        .state = command.state,
        .context = command.context,
    };
}

PrCommandIndex prCommandRegistryFind(const PrCommandRegistry* registry, PrStringView id) {
    for (uint32_t i = 0; i < registry->commands.count; ++i) {
        if (viewOf(registry->commands.data[i]->id) == id)
            return i;
    }
    return kPrCommandMissing;
}

PrCommandStateFlags prCommandRegistryState(const PrCommandRegistry* registry, PrCommandIndex index) {
    const Command& command = commandAt(registry, index);
    return command.state != nullptr ? command.state(command.context) : 0;
}

bool prCommandRegistryIsDirect(const PrCommandRegistry* registry, PrCommandIndex index) {
    const Command& command = commandAt(registry, index);
    for (uint32_t i = 0; i < command.paramCount; ++i) {
        if (command.params[i].required)
            return false;
    }
    return true;
}

PrStatus prCommandRegistryRun(const PrCommandRegistry* registry, PrStringView id, const PrCommandArgs* PR_NULLABLE args,
        PrString* PR_NULLABLE error) {
    PrCommandIndex index = prCommandRegistryFind(registry, id);
    if (index == kPrCommandMissing) {
        explain(error, id, "does not exist");
        return PrStatus_NotFound;
    }
    if ((prCommandRegistryState(registry, index) & PrCommandStateFlag_Disabled) != 0) {
        explain(error, id, "is disabled");
        return PrStatus_InvalidState;
    }
    PrCommandArgs none{.object = kPrJsonMissing};
    const PrCommandArgs* given = args != nullptr ? args : &none;
    const Command& command = commandAt(registry, index);
    PrStatus status = checkArgs(command, given, error);
    if (status == PrStatus_Ok)
        command.run(command.context, given);
    return status;
}

bool prCommandArgsHas(const PrCommandArgs* args, PrStringView name) {
    return argument(args, name) != kPrJsonMissing;
}

bool prCommandArgsBool(const PrCommandArgs* args, PrStringView name, bool fallback) {
    PrJsonValueIndex value = argument(args, name);
    PrJsonKind kind = value != kPrJsonMissing ? prJsonKind(args->document, value) : PrJsonKind_Null;
    return kind == PrJsonKind_True || (kind != PrJsonKind_False && fallback);
}

double prCommandArgsNumber(const PrCommandArgs* args, PrStringView name, double fallback) {
    PrJsonValueIndex value = argument(args, name);
    if (value == kPrJsonMissing || prJsonKind(args->document, value) != PrJsonKind_Number)
        return fallback;
    return prJsonNumber(args->document, value, fallback);
}

PrStringView prCommandArgsString(const PrCommandArgs* args, PrStringView name, PrStringView fallback) {
    PrJsonValueIndex value = argument(args, name);
    if (value == kPrJsonMissing || prJsonKind(args->document, value) != PrJsonKind_String)
        return fallback;
    return prJsonString(args->document, value);
}
