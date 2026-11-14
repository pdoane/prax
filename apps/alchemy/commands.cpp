// Alchemy's commands and key bindings.

#include "apps/alchemy/commands.h"

#include "pr/array.h"
#include "pr/ascii.h"
#include "pr/count_of.h"
#include "pr/lock.h"
#include "pr/min_max.h"
#include "pr/string.h"
#include "pr/ui_text_edit.h"

#include "apps/alchemy/prompt_text.h"

namespace alchemy {

using namespace pr;

struct CommandSpec;

// The context that the registry passes to a command: the shell and the command's spec.
struct CommandContext {
    Shell* shell;
    const CommandSpec* spec;
};

// A command's implementation: what it does with the shell and its arguments, and its state, if it has one.
typedef void (*RunFn)(Shell& shell, const PrCommandArgs* args);
typedef PrCommandStateFlags (*StateFn)(Shell& shell);

// A command of the table, and the chord that binds it, if any, in its context.
struct CommandSpec {
    const char* id;
    const char* title;
    const char* category;
    RunFn run;
    StateFn PR_NULLABLE state = nullptr;
    const char* chord = nullptr;
    PrStringView context = "";
    PrCommandBindingKind kind = PrCommandBindingKind_Normal;
    const PrCommandParam* PR_NULLABLE params = nullptr;
    uint32_t paramCount = 0;
    PrCommandFlags flags = 0;
    const char* description = "";
};

namespace {

// The platform's chord for fullscreen: Control-Command-F on Apple's platforms, and F11 elsewhere.
#if defined(__APPLE__)
constexpr const char* kFullscreenChord = "Ctrl Cmd F";
#else
constexpr const char* kFullscreenChord = "F11";
#endif

// Makes the window fullscreen, or a Normal window again.
void toggleFullscreen(Shell& s) {
    if (s.window == nullptr)
        return;
    bool full = prAppWindowState(s.window) == PrAppWindowState_Fullscreen;
    prAppWindowSetState(s.window, full ? PrAppWindowState_Normal : PrAppWindowState_Fullscreen);
}

PrCommandStateFlags disabledUnless(bool enabled) {
    return enabled ? 0 : PrCommandStateFlag_Disabled;
}

PrCommandStateFlags whenConnected(Shell& s) {
    return disabledUnless(s.state->connection == ConnectionState_Connected);
}

PrCommandStateFlags whenSelected(Shell& s) {
    return disabledUnless(gallerySelected(s.state->gallery) != nullptr);
}

PrCommandStateFlags whenRecorded(Shell& s) {
    return disabledUnless(selectedRecord(*s.state) != nullptr);
}

// Returns the prompt field that holds the keyboard, as an index into the shell's fields, or -1.
int focusedPrompt(const Shell& s) {
    if (s.ui == nullptr)
        return -1;
    for (int i = 0; i < 2; ++i) {
        if (s.promptFields[i] != kPrLayoutNodeNone && prUiIsFocusWithin(s.ui, s.promptFields[i]))
            return i;
    }
    return -1;
}

String& promptText(Shell& s, int field) {
    return field == 0 ? s.state->params.prompt : s.state->params.negativePrompt;
}

// Adds `steps` presses of the weight chords to the attention of the run at the focused prompt's caret.
void adjustWeight(Shell& s, int steps) {
    int field = focusedPrompt(s);
    if (field < 0)
        return;
    PrUi* ui = s.ui;
    String& text = promptText(s, field);
    PrUiTextSelection selection = prUiTextEditSelection(ui, s.promptFields[field]);
    uint32_t start = min(selection.anchor, selection.caret);
    uint32_t end = max(selection.anchor, selection.caret);
    WeightEdit edit;
    if (!promptWeightAdjust(text, start, end, steps * kPromptWeightStep, &edit))
        return;
    text.replace(edit.begin, edit.end - edit.begin, edit.text);
    uint32_t written = edit.begin + edit.text.count();
    prUiTextEditSetSelection(ui, s.promptFields[field], PrUiTextSelection{.anchor = edit.begin, .caret = written});
}

// The parameters of alchemy.insertEmbedding and alchemy.use.
constexpr PrCommandParam kInsertEmbedding[] = {
    {.name = "name", .kind = PrCommandParamKind_String, .required = true, .description = "A part of an embedding's name"},
    {.name = "negative", .kind = PrCommandParamKind_Bool, .description = "Insert into the negative prompt"},
};
constexpr PrCommandParam kUse[] = {
    {
        .name = "operation",
        .kind = PrCommandParamKind_Number,
        .required = true,
        .description = "The operation of the record, from the oldest",
    },
    {
        .name = "what",
        .kind = PrCommandParamKind_String,
        .required = true,
        .description = "prompt, negative, promptLine, negativeLine, model, loras, sampling, aspect, seed, strength, "
            "upscale, outpaint, detector, source or all",
    },
    {.name = "line", .kind = PrCommandParamKind_Number, .description = "The line of promptLine or negativeLine"},
};

void runInsertEmbedding(Shell& s, const PrCommandArgs* args) {
    PrStringView part = prCommandArgsString(args, "name", "");
    for (const String& name : s.state->catalog.embeddings) {
        if (prAsciiContainsFold(name, part)) {
            insertEmbedding(s, prCommandArgsBool(args, "negative", false) ? 1 : 0, name);
            return;
        }
    }
    setStatus(*s.state, "No embedding on the server matches that name.");
}

void runUse(Shell& s, const PrCommandArgs* args) {
    UseWhat what = parseUseWhat(prCommandArgsString(args, "what", ""));
    double operation = prCommandArgsNumber(args, "operation", -1);
    double line = prCommandArgsNumber(args, "line", 0);
    if (what == UseWhat_Count || operation < 0 || line < 0 ||
            !useRecorded(*s.state, uint32_t(operation), what, uint32_t(line)))
        setStatus(*s.state, "The record does not state that.");
}

void copyImage(Shell& s) {
    Array<uint8_t> pixels;
    PrUVec2 extent = selectedPixels(*s.state, &pixels);
    PrAppDataContent content{
        .forms = PrAppDataFormFlag_Image,
        .image = PrPixelView{pixels.data(), size_t(extent.x) * 4, extent},
    };
    bool copied = extent.x != 0 && s.app != nullptr &&
        prAppDataWrite(s.app, kPrAppDataSourceClipboard, &content) == PrStatus_Ok;
    setStatus(*s.state, copied ? "Copied the image." : "The image could not be copied.");
}

// Stages the clipboard's image in the import's dialog, and requests the frame that shows it.
void imageRead(void* PR_NULLABLE context, PrStatus status, PrPixelView pixels) {
    Shell& s = *static_cast<Shell*>(context);
    if (status == PrStatus_Ok) {
        stageImport(*s.state, pixels);
    } else {
        s.state->import.open = true;
        s.state->import.message = "The clipboard could not be read.";
    }
    if (s.window != nullptr)
        prAppWindowRequestFrame(s.window);
}

void pasteImage(Shell& s) {
    openImport(*s.state);
    if (s.app != nullptr)
        prAppDataReadImage(s.app, kPrAppDataSourceClipboard, imageRead, &s);
}

void setMode(Shell& s, OperationMode mode) {
    s.state->mode = mode;
}

PrCommandStateFlags checkedIf(bool checked) {
    return checked ? PrCommandStateFlag_Checked : 0;
}

// The parameters that alchemy.setParameters takes, which automation uses to describe a run.
constexpr PrCommandParam kParameters[] = {
    {.name = "mode", .kind = PrCommandParamKind_String, .description = "generate, upscale, outpaint or detail"},
    {.name = "prompt", .kind = PrCommandParamKind_String, .description = "The positive prompt"},
    {.name = "negative", .kind = PrCommandParamKind_String, .description = "The negative prompt"},
    {.name = "checkpoint", .kind = PrCommandParamKind_String, .description = "A part of a checkpoint's name"},
    {.name = "sampler", .kind = PrCommandParamKind_String, .description = "A sampler's name"},
    {.name = "scheduler", .kind = PrCommandParamKind_String, .description = "A scheduler's name"},
    {.name = "steps", .kind = PrCommandParamKind_Number, .description = "Sampling steps, 1 to 150"},
    {.name = "cfg", .kind = PrCommandParamKind_Number, .description = "Guidance scale, 1 to 30"},
    {.name = "seed", .kind = PrCommandParamKind_Number, .description = "The seed, which locks it"},
    {.name = "batch", .kind = PrCommandParamKind_Number, .description = "Images per run, 1 to 8"},
    {.name = "width", .kind = PrCommandParamKind_Number, .description = "Width in pixels, which overrides the ladder"},
    {.name = "height", .kind = PrCommandParamKind_Number, .description = "Height in pixels, which overrides the ladder"},
};

// Returns the first of `names` that contains `part`, or `current` if none does.
String matchName(const Array<String>& names, PrStringView part, PrStringView current) {
    for (const String& name : names) {
        if (prAsciiContainsFold(name, part))
            return name;
    }
    return current;
}

void setParameters(Shell& s, const PrCommandArgs* args) {
    AppState& st = *s.state;
    GenerateParams& p = st.params;
    if (prCommandArgsHas(args, "mode"))
        st.mode = parseMode(prCommandArgsString(args, "mode", ""), st.mode);
    if (prCommandArgsHas(args, "prompt"))
        p.prompt = prCommandArgsString(args, "prompt", "");
    if (prCommandArgsHas(args, "negative"))
        p.negativePrompt = prCommandArgsString(args, "negative", "");
    if (prCommandArgsHas(args, "checkpoint"))
        p.checkpoint = matchName(st.catalog.checkpoints, prCommandArgsString(args, "checkpoint", ""), p.checkpoint);
    if (prCommandArgsHas(args, "sampler"))
        p.sampler = matchName(st.catalog.samplers, prCommandArgsString(args, "sampler", ""), p.sampler);
    if (prCommandArgsHas(args, "scheduler"))
        p.scheduler = matchName(st.catalog.schedulers, prCommandArgsString(args, "scheduler", ""), p.scheduler);
    if (prCommandArgsHas(args, "steps"))
        p.steps = uint32_t(clamp(prCommandArgsNumber(args, "steps", p.steps), 1.0, 150.0));
    if (prCommandArgsHas(args, "cfg"))
        p.cfg = clamp(prCommandArgsNumber(args, "cfg", p.cfg), 1.0, 30.0);
    if (prCommandArgsHas(args, "seed")) {
        double seed = prCommandArgsNumber(args, "seed", 0);
        p.seed = seed > 0 && seed < 9007199254740992.0 ? uint64_t(seed) : 0;
        p.seedLocked = true;
    }
    if (prCommandArgsHas(args, "batch"))
        p.batch = uint32_t(clamp(prCommandArgsNumber(args, "batch", p.batch), 1.0, 8.0));
    if (prCommandArgsHas(args, "width") || prCommandArgsHas(args, "height")) {
        setCustomSize(st.dims, true);
        st.dims.size.width = uint32_t(clamp(prCommandArgsNumber(args, "width", st.dims.size.width), 64.0, 4096.0));
        st.dims.size.height = uint32_t(clamp(prCommandArgsNumber(args, "height", st.dims.size.height), 64.0, 4096.0));
    }
}

void reportStatus(Shell& s, const PrCommandArgs* args) {
    if (args->result == nullptr)
        return;
    AppState& st = *s.state;
    PrJsonWriter* w = args->result;
    prJsonWriterBeginObject(w);
    prJsonWriterKey(w, "connected");
    prJsonWriterBool(w, st.connection == ConnectionState_Connected);
    prJsonWriterKey(w, "status");
    prJsonWriterString(w, st.status);
    prJsonWriterKey(w, "runs");
    prJsonWriterNumber(w, runsInFlight(st));
    prJsonWriterKey(w, "images");
    prJsonWriterNumber(w, double(st.gallery.entries.count()));
    const GalleryEntry* selected = gallerySelected(st.gallery);
    prJsonWriterKey(w, "selected");
    prJsonWriterString(w, selected != nullptr ? selected->path : String());
    prJsonWriterEndObject(w);
}

constexpr CommandSpec kSpecs[] = {
    // Runs.
    {
        .id = "alchemy.generate",
        .title = "Generate",
        .category = "Run",
        .run = [](Shell& s, const PrCommandArgs*) { generate(*s.state); },
        .state = whenConnected,
        .chord = "Cmd Enter",
    },
    {
        .id = "alchemy.interrupt",
        .title = "Stop the Run",
        .category = "Run",
        .run = [](Shell& s, const PrCommandArgs*) { interruptRun(*s.state); },
        .state = [](Shell& s) { return disabledUnless(runsInFlight(*s.state) != 0); },
        .chord = "Cmd .",
    },
    {
        .id = "alchemy.connect",
        .title = "Connect to the Server",
        .category = "Server",
        .run = [](Shell& s, const PrCommandArgs*) { connectServer(*s.state); },
        .chord = "Cmd Shift C",
    },
    {
        .id = "alchemy.disconnect",
        .title = "Disconnect",
        .category = "Server",
        .run = [](Shell& s, const PrCommandArgs*) { disconnectServer(*s.state); },
        .state = whenConnected,
    },
    {
        .id = "alchemy.mode.generate",
        .title = "Generate Mode",
        .category = "Mode",
        .run = [](Shell& s, const PrCommandArgs*) { setMode(s, OperationMode_Generate); },
        .state = [](Shell& s) { return checkedIf(s.state->mode == OperationMode_Generate); },
        .chord = "Cmd 1",
    },
    {
        .id = "alchemy.mode.upscale",
        .title = "Upscale Mode",
        .category = "Mode",
        .run = [](Shell& s, const PrCommandArgs*) { setMode(s, OperationMode_Upscale); },
        .state = [](Shell& s) { return checkedIf(s.state->mode == OperationMode_Upscale); },
        .chord = "Cmd 2",
    },
    {
        .id = "alchemy.mode.outpaint",
        .title = "Outpaint Mode",
        .category = "Mode",
        .run = [](Shell& s, const PrCommandArgs*) { setMode(s, OperationMode_Outpaint); },
        .state = [](Shell& s) { return checkedIf(s.state->mode == OperationMode_Outpaint); },
        .chord = "Cmd 3",
    },
    {
        .id = "alchemy.mode.detail",
        .title = "Detail Mode",
        .category = "Mode",
        .run = [](Shell& s, const PrCommandArgs*) { setMode(s, OperationMode_Detail); },
        .state = [](Shell& s) { return checkedIf(s.state->mode == OperationMode_Detail); },
        .chord = "Cmd 4",
    },

    // Parameters.
    {
        .id = "alchemy.newSeed",
        .title = "New Seed",
        .category = "Parameters",
        .run = [](Shell& s, const PrCommandArgs*) {
            s.state->params.seed = drawSeed(*s.state);
            s.state->params.seedLocked = true;
        },
    },
    {
        .id = "alchemy.lockSeed",
        .title = "Lock the Seed",
        .category = "Parameters",
        .run = [](Shell& s, const PrCommandArgs*) { s.state->params.seedLocked = !s.state->params.seedLocked; },
        .state = [](Shell& s) { return checkedIf(s.state->params.seedLocked); },
    },
    {
        .id = "alchemy.swapDimensions",
        .title = "Swap Width and Height",
        .category = "Parameters",
        .run = [](Shell& s, const PrCommandArgs*) { swapDimensions(s.state->dims); },
    },
    {
        .id = "alchemy.clearSource",
        .title = "Clear the Source Image",
        .category = "Parameters",
        .run = [](Shell& s, const PrCommandArgs*) { clearSource(*s.state); },
        .state = [](Shell& s) { return disabledUnless(!s.state->params.sourcePath.isEmpty()); },
    },
    {
        .id = "alchemy.increaseWeight",
        .title = "Increase Attention",
        .category = "Prompt",
        .run = [](Shell& s, const PrCommandArgs*) { adjustWeight(s, 1); },
        .chord = "Cmd Alt Up",
        .context = kContextParameters,
        .flags = PrCommandFlag_Hidden,
    },
    {
        .id = "alchemy.decreaseWeight",
        .title = "Decrease Attention",
        .category = "Prompt",
        .run = [](Shell& s, const PrCommandArgs*) { adjustWeight(s, -1); },
        .chord = "Cmd Alt Down",
        .context = kContextParameters,
        .flags = PrCommandFlag_Hidden,
    },
    {
        .id = "alchemy.setParameters",
        .title = "Set Parameters",
        .category = "Parameters",
        .run = setParameters,
        .params = kParameters,
        .paramCount = uint32_t(countOf(kParameters)),
        .description = "Sets the parameters of the next run. Each argument that is given replaces one parameter.",
    },
    {
        .id = "alchemy.status",
        .title = "Status",
        .category = "Server",
        .run = reportStatus,
        .flags = PrCommandFlag_Hidden | PrCommandFlag_Query,
        .description = "Reports the connection, the status line, the runs in flight and the selected image.",
    },

    {
        .id = "alchemy.insertEmbedding",
        .title = "Insert Embedding",
        .category = "Prompt",
        .run = runInsertEmbedding,
        .params = kInsertEmbedding,
        .paramCount = uint32_t(countOf(kInsertEmbedding)),
        .description = "Writes the token of the first embedding whose name holds `name` into a prompt.",
    },
    {
        .id = "alchemy.configureServer",
        .title = "Server...",
        .category = "Server",
        .run = [](Shell& s, const PrCommandArgs*) { s.state->serverOpen = true; },
    },

    // The gallery.
    {
        .id = "alchemy.back",
        .title = "Back",
        .category = "Image",
        .run = [](Shell& s, const PrCommandArgs*) { historyBack(*s.state); },
        .state = [](Shell& s) { return disabledUnless(canGoBack(*s.state)); },
        .chord = "Cmd [",
    },
    {
        .id = "alchemy.forward",
        .title = "Forward",
        .category = "Image",
        .run = [](Shell& s, const PrCommandArgs*) { historyForward(*s.state); },
        .state = [](Shell& s) { return disabledUnless(canGoForward(*s.state)); },
        .chord = "Cmd ]",
    },
    {
        .id = "alchemy.previousImage",
        .title = "Previous Image",
        .category = "Image",
        .run = [](Shell& s, const PrCommandArgs*) { galleryStep(s.state->gallery, -1); },
        .chord = "Left",
        .context = kContextGallery,
    },
    {
        .id = "alchemy.nextImage",
        .title = "Next Image",
        .category = "Image",
        .run = [](Shell& s, const PrCommandArgs*) { galleryStep(s.state->gallery, 1); },
        .chord = "Right",
        .context = kContextGallery,
    },
    {
        .id = "alchemy.useAsSource",
        .title = "Use as Source",
        .category = "Image",
        .run = [](Shell& s, const PrCommandArgs*) { useAsSource(*s.state); },
        .state = whenSelected,
    },
    {
        .id = "alchemy.usePrompt",
        .title = "Use Prompt",
        .category = "Image",
        .run = [](Shell& s, const PrCommandArgs*) { usePrompt(*s.state); },
        .state = whenRecorded,
    },
    {
        .id = "alchemy.useSeed",
        .title = "Use Seed",
        .category = "Image",
        .run = [](Shell& s, const PrCommandArgs*) { useSeed(*s.state); },
        .state = whenRecorded,
    },
    {
        .id = "alchemy.useAspect",
        .title = "Use Aspect Ratio",
        .category = "Image",
        .run = [](Shell& s, const PrCommandArgs*) { useAspect(*s.state); },
        .state = whenRecorded,
    },
    {
        .id = "alchemy.useAll",
        .title = "Use All Settings",
        .category = "Image",
        .run = [](Shell& s, const PrCommandArgs*) { useAll(*s.state); },
        .state = whenRecorded,
    },
    {
        .id = "alchemy.use",
        .title = "Use Recorded Settings",
        .category = "Image",
        .run = runUse,
        .state = whenRecorded,
        .params = kUse,
        .paramCount = uint32_t(countOf(kUse)),
        .flags = PrCommandFlag_Hidden,
        .description = "Carries a group of one operation of the selected image's record into the parameters.",
    },
    {
        .id = "alchemy.copyImage",
        .title = "Copy Image",
        .category = "Image",
        .run = [](Shell& s, const PrCommandArgs*) { copyImage(s); },
        .state = whenSelected,
        .chord = "Cmd C",
        .context = kContextGallery,
    },
    {
        .id = "alchemy.importImage",
        .title = "Import Image...",
        .category = "Image",
        .run = [](Shell& s, const PrCommandArgs*) { openImport(*s.state); },
    },
    {
        .id = "alchemy.pasteImage",
        .title = "Paste Image...",
        .category = "Image",
        .run = [](Shell& s, const PrCommandArgs*) { pasteImage(s); },
        .chord = "Cmd V",
        .context = kContextGallery,
    },
    {
        .id = "alchemy.revealSource",
        .title = "Reveal the Source in the Browser",
        .category = "Image",
        .run = [](Shell& s, const PrCommandArgs*) {
            gallerySelect(s.state->gallery, galleryIndexOf(s.state->gallery, s.state->params.sourcePath));
        },
        .state = [](Shell& s) {
            const Gallery& gallery = s.state->gallery;
            return disabledUnless(galleryIndexOf(gallery, s.state->params.sourcePath) < gallery.entries.count());
        },
    },
    {
        .id = "alchemy.deleteImage",
        .title = "Delete Image...",
        .category = "Image",
        .run = [](Shell& s, const PrCommandArgs*) { requestDelete(*s.state); },
        .state = whenSelected,
        .chord = "Cmd Backspace",
        .kind = PrCommandBindingKind_Override,
    },
    {
        .id = "alchemy.rescan",
        .title = "Rescan the Gallery",
        .category = "Image",
        .run = [](Shell& s, const PrCommandArgs*) { galleryRescan(s.state->gallery); },
    },
    {
        .id = "alchemy.openGallery",
        .title = "Open the Gallery Folder",
        .category = "Image",
        .run = [](Shell& s, const PrCommandArgs*) {
            if (s.app != nullptr)
                prAppOpenUrl(s.app, format("file://{}", s.state->gallery.dir));
        },
    },

    // The view.
    {
        .id = "alchemy.toggleInfo",
        .title = "Image Info",
        .category = "View",
        .run = [](Shell& s, const PrCommandArgs*) { s.state->showInfo = !s.state->showInfo; },
        .state = [](Shell& s) { return checkedIf(s.state->showInfo); },
        .chord = "I",
        .context = kContextGallery,
    },
    {
        .id = "alchemy.togglePreview",
        .title = "Live Preview",
        .category = "View",
        .run = [](Shell& s, const PrCommandArgs*) { s.state->showPreview = !s.state->showPreview; },
        .state = [](Shell& s) { return checkedIf(s.state->showPreview); },
        .chord = "P",
        .context = kContextGallery,
    },
    {
        .id = "view.fullscreen",
        .title = "Full Screen",
        .category = "View",
        .run = [](Shell& s, const PrCommandArgs*) { toggleFullscreen(s); },
        .state = [](Shell& s) {
            return checkedIf(s.window != nullptr && prAppWindowState(s.window) == PrAppWindowState_Fullscreen);
        },
        .chord = kFullscreenChord,
    },
    {
        .id = "view.palette",
        .title = "Command Palette...",
        .category = "View",
        .run = [](Shell& s, const PrCommandArgs*) { s.paletteRequested = true; },
        .chord = "Cmd Shift P",
        .flags = PrCommandFlag_Hidden,
    },
    {
        .id = "app.quit",
        .title = "Quit",
        .category = "App",
        .run = [](Shell& s, const PrCommandArgs*) {
            if (s.app != nullptr)
                prAppStop(s.app);
        },
        .chord = "Cmd Q",
    },
};

constexpr size_t kSpecCount = countOf(kSpecs);

void runBinding(void* PR_NULLABLE context, const PrCommandArgs* args) {
    const CommandContext* binding = static_cast<const CommandContext*>(context);
    binding->spec->run(*binding->shell, args);
}

PrCommandStateFlags stateBinding(void* PR_NULLABLE context) {
    const CommandContext* binding = static_cast<const CommandContext*>(context);
    return binding->spec->state != nullptr ? binding->spec->state(*binding->shell) : 0;
}

} // namespace

Commands* commandsCreate(Shell* shell) {
    CommandContext* contexts = new CommandContext[kSpecCount];
    Commands* commands = new Commands{prCommandRegistryCreate(), prCommandKeymapCreate(), shell, contexts};
    for (size_t i = 0; i < kSpecCount; ++i) {
        const CommandSpec& spec = kSpecs[i];
        contexts[i] = CommandContext{shell, &spec};
        PrCommandDesc desc{
            .id = spec.id,
            .title = spec.title,
            .category = spec.category,
            .description = spec.description,
            .flags = spec.flags,
            .params = spec.params,
            .paramCount = spec.paramCount,
            .run = runBinding,
            .state = stateBinding,
            .context = &contexts[i],
        };
        prCommandRegistryAdd(commands->registry, &desc);
        PrCommandChord chord{};
        if (spec.chord != nullptr && prCommandChordParse(spec.chord, &chord))
            prCommandKeymapBind(commands->keymap, chord, spec.id, spec.context, spec.kind);
    }
    // The dialog of an import takes the same paste as the gallery.
    PrCommandChord paste{};
    if (prCommandChordParse("Cmd V", &paste))
        prCommandKeymapBind(commands->keymap, paste, "alchemy.pasteImage", kContextImport, PrCommandBindingKind_Normal);
    return commands;
}

void commandsDestroy(Commands* PR_NULLABLE commands) {
    if (commands == nullptr)
        return;
    prCommandKeymapDestroy(commands->keymap);
    prCommandRegistryDestroy(commands->registry);
    delete[] commands->contexts;
    delete commands;
}

void insertEmbedding(Shell& shell, int field, PrStringView filename) {
    String& text = promptText(shell, field);
    uint32_t start = text.count();
    uint32_t end = start;
    PrLayoutNode node = shell.promptFields[field];
    bool placed = shell.ui != nullptr && node != kPrLayoutNodeNone && shell.promptTouched[field];
    if (placed) {
        PrUiTextSelection selection = prUiTextEditSelection(shell.ui, node);
        start = min(min(selection.anchor, selection.caret), start);
        end = min(max(selection.anchor, selection.caret), end);
    }
    String token = embeddingInsertText(text, start, end, filename);
    text.replace(start, end - start, token);
    if (shell.ui != nullptr && node != kPrLayoutNodeNone) {
        uint32_t caret = start + token.count();
        prUiTextEditSetSelection(shell.ui, node, PrUiTextSelection{.anchor = caret, .caret = caret});
        prUiSetFocus(shell.ui, node);
    }
}

void runCommand(const Commands& commands, PrStringView id) {
    PrCommandIndex index = prCommandRegistryFind(commands.registry, id);
    if (index == kPrCommandMissing || (prCommandRegistryState(commands.registry, index) & PrCommandStateFlag_Disabled))
        return;
    prCommandRegistryRun(commands.registry, id, nullptr, nullptr);
}

} // namespace alchemy
