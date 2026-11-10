// Alchemy's interface.

#include "apps/alchemy/view.h"

#include "pr/array.h"
#include "pr/command_kit.h"
#include "pr/file.h"
#include "pr/float.h"
#include "pr/kit_box.h"
#include "pr/kit_button.h"
#include "pr/kit_choice.h"
#include "pr/kit_collection.h"
#include "pr/kit_image.h"
#include "pr/kit_input.h"
#include "pr/kit_menu.h"
#include "pr/kit_panel.h"
#include "pr/kit_text.h"
#include "pr/min_max.h"
#include "pr/string.h"
#include "pr/ui_text_edit.h"

#include "apps/alchemy/prompt_text.h"

namespace alchemy {

namespace {

// The dock's panels, which are fixed for the life of the window.
enum {
    Panel_Parameters = 1,
    Panel_Viewport,
    Panel_Browser,
};
typedef uint8_t Panel;

constexpr float kTileWidth = 132;
constexpr float kTileHeight = 150;
constexpr float kTileHoverZoom = 0.12f;

constexpr float kSourceThumbnail = 64;
constexpr float kRecordWidth = 360;
constexpr float kRecordMaxHeight = 520;
constexpr float kImportPreview = 360;
constexpr float kMenuWidth = 200;

// The browser's tab counts the gallery's images.
PrKitDockTab describePanel(void* PR_NULLABLE context, PrKitDockPanelId panel) {
    switch (panel) {
        case Panel_Parameters: return {.title = "Parameters"};
        case Panel_Browser: return {.title = static_cast<const View*>(context)->browserTitle};
        default: return {.title = "Viewport"};
    }
}

PrKitTextStyle textStyle(PrKitTextRole role, PrKitTone tone = PrKitTone_Auto) {
    return PrKitTextStyle{.role = role, .tone = tone, .wrap = PrKitWrap_Clip};
}

PrLayoutNode row(PrKit* kit, PrLayoutNode parent, float gap = kPrKitSpace3) {
    PrKitBoxStyle style{.direction = PrKitDirection_Row, .align = PrKitAlign_Center, .gap = gap};
    return prKitBox(kit, parent, {}, style);
}

// A button that runs the command `id`, disabled while the command is and on while it is checked.
PrLayoutNode commandButton(PrKit* kit, PrLayoutNode parent, Commands& commands, PrStringView id, PrStringView text,
        PrKitIconKind icon, PrKitButtonVariant variant = PrKitButtonVariant_Ghost,
        PrKitControlSize size = PrKitControlSize_Sm) {
    PrCommandIndex index = prCommandRegistryFind(commands.registry, id);
    PrCommandStateFlags state = index != kPrCommandMissing ? prCommandRegistryState(commands.registry, index) : 0;
    PrKitButton button{
        .key = prUiKey(kit->ui, parent, id),
        .text = text,
        .icon = icon,
        .selected = (state & PrCommandStateFlag_Checked) != 0,
        .disabled = index == kPrCommandMissing || (state & PrCommandStateFlag_Disabled) != 0,
    };
    PrLayoutNode node = prKitButton(kit, parent, button, {.variant = variant, .size = size});
    prCommandTooltip(kit, node, commands.registry, commands.keymap, id);
    if (prKitActivated(kit, node))
        runCommand(commands, id);
    return node;
}

// A small button of an icon alone, with a tooltip.
PrLayoutNode iconButton(PrKit* kit, PrLayoutNode parent, PrKitIconKind icon, PrStringView tooltip, bool selected = false) {
    PrLayoutNode node = prKitButton(kit, parent, {.icon = icon, .selected = selected},
        {.variant = PrKitButtonVariant_Ghost, .size = PrKitControlSize_Sm});
    prKitTooltip(kit, node, {.text = tooltip});
    return node;
}

// A select of `names` that writes the chosen name to `current`. An empty list shows `missing`.
void selectName(PrKit* kit, PrLayoutNode parent, const Array<String>& names, String& current,
        PrStringView missing, PrKitFlex flex = PrKitFlex_Fill) {
    PrStringView label = names.isEmpty() ? missing : current.isEmpty() ? PrStringView("Choose...") : current.view();
    PrLayoutNode select = prKitSelect(kit, parent, {.label = label, .disabled = names.isEmpty()}, {});
    PrKitPlace place{.flex = flex};
    prKitPlace(kit, select, &place);
    int32_t step = 0;
    if (prKitStepped(kit, select, &step) && !names.isEmpty()) {
        uint32_t at = names.find(current);
        int64_t index = at == kArrayMissing ? 0 : int64_t(at) + step;
        current = names[uint32_t(clamp<int64_t>(index, 0, int64_t(names.count()) - 1))];
    }
    PrLayoutNode menu = prKitSelectMenu(kit, select);
    if (menu == kPrLayoutNodeNone)
        return;
    for (uint32_t i = 0; i < names.count(); ++i) {
        PrKitMenuItem item{.text = names[i], .checked = names[i] == current};
        if (prKitActivated(kit, prKitMenuItem(kit, menu, item, {})))
            current = names[i];
    }
}

PrLayoutNode field(PrKit* kit, PrLayoutNode parent, const char* label) {
    return prKitInspectorRow(kit, parent, {.label = label}, {});
}

void numberField(PrKit* kit, PrLayoutNode parent, PrKitScalar value, double min, double max, uint8_t precision = 0) {
    PrKitNumber number{
        .value = value,
        .range = {.min = min, .max = max, .hasMin = true, .hasMax = true},
        .precision = precision,
    };
    prKitNumberField(kit, parent, number, {});
}

// A column of a caption over the control that the caller creates under it. `flex` places it in its parent: Fill shares
// a row's width, and Auto takes its own height in a column.
PrLayoutNode captioned(PrKit* kit, PrLayoutNode parent, const char* label, PrKitFlex flex = PrKitFlex_Auto) {
    PrKitBoxStyle style{.gap = kPrKitSpace1, .place = {.flex = flex}};
    PrLayoutNode column = prKitBox(kit, parent, {}, style);
    prKitLabel(kit, column, {.text = label}, textStyle(PrKitTextRole_Caption, PrKitTone_Muted));
    return column;
}

void captionedSelect(PrKit* kit, PrLayoutNode parent, const char* label, const Array<String>& names, String& current) {
    selectName(kit, captioned(kit, parent, label, PrKitFlex_Fill), names, current, "None", PrKitFlex_Auto);
}

void captionedNumber(PrKit* kit, PrLayoutNode parent, const char* label, PrKitScalar value, double min, double max,
        uint8_t precision = 0) {
    numberField(kit, captioned(kit, parent, label, PrKitFlex_Fill), value, min, max, precision);
}

void buildMode(PrKit* kit, PrLayoutNode parent, AppState& state) {
    static const char* const kLabels[] = {"Generate", "Upscale", "Outpaint", "Detail"};
    PrLayoutNode modes = prKitSegmented(kit, parent, {}, {});
    for (OperationMode mode = 0; mode < OperationMode_Count; ++mode) {
        PrKitChoice choice{.label = kLabels[mode], .selected = state.mode == mode};
        if (prKitActivated(kit, prKitSegment(kit, modes, choice)))
            state.mode = mode;
    }
}

// The source image that the runs work over, which shows only while one is held. A mode that cannot run without one says
// how to choose it.
void buildSource(PrKit* kit, PrLayoutNode parent, AppState& state, Commands& commands) {
    GenerateParams& p = state.params;
    if (p.sourcePath.isEmpty()) {
        if (modeNeedsSource(state.mode)) {
            prKitLabel(kit, parent, {.text = "Right-click an image and choose Use as Source."},
                PrKitTextStyle{.role = PrKitTextRole_BodySmall, .tone = PrKitTone_Faint, .wrap = PrKitWrap_Wrap});
        }
        return;
    }
    PrLayoutNode line = row(kit, parent);
    PrKitBoxStyle frame{
        .justify = PrKitJustify_Center,
        .align = PrKitAlign_Center,
        .overflow = PrKitOverflow_Clip,
        .place = {.width = prKitPx(kSourceThumbnail), .height = prKitPx(kSourceThumbnail), .flex = PrKitFlex_Fixed},
    };
    PrLayoutNode thumbnail = prKitBox(kit, line, {}, frame);
    if (const PrDrawImage* image = imageCacheGet(state.images, p.sourcePath, ImageScale_Thumbnail)) {
        PrKitImageStyle contain{
            .fit = PrKitImageFit_Contain,
            .place = {.width = prKitPercent(100), .height = prKitPercent(100)},
        };
        prKitImage(kit, thumbnail, {.image = image}, contain);
    } else {
        prKitIcon(kit, thumbnail, {.icon = PrKitIconKind_Image}, {.size = 20, .tone = PrKitTone_Faint});
    }
    PrKitBoxStyle details{.gap = kPrKitSpace2, .place = {.flex = PrKitFlex_Fill}};
    PrLayoutNode column = prKitBox(kit, line, {}, details);
    prKitLabel(kit, column, {.text = prFilePathName(p.sourcePath)},
        textStyle(PrKitTextRole_BodySmall, PrKitTone_Secondary));
    PrLayoutNode actions = row(kit, column);
    commandButton(kit, actions, commands, "alchemy.clearSource", "Clear", PrKitIconKind_None,
        PrKitButtonVariant_Default, PrKitControlSize_Xs);
    PrLayoutNode menu = prKitContextMenu(kit, thumbnail, {}, {.minWidth = kMenuWidth});
    if (menu != kPrLayoutNodeNone) {
        prCommandMenuItem(kit, menu, commands.registry, commands.keymap, "alchemy.revealSource", {});
        prCommandMenuItem(kit, menu, commands.registry, commands.keymap, "alchemy.clearSource", {});
    }
}

// The caption of a prompt and the menu of the server's embeddings, which write their tokens into the field below it.
void buildPromptHeader(PrKit* kit, PrLayoutNode parent, AppState& state, Shell& shell, const char* label, int target) {
    PrLayoutNode header = row(kit, parent);
    prKitLabel(kit, header, {.text = label}, textStyle(PrKitTextRole_Caption, PrKitTone_Muted));
    prKitSpacer(kit, header, {});
    PrLayoutNode add = iconButton(kit, header, PrKitIconKind_Plus, "Insert an embedding");
    if (prKitActivated(kit, add))
        prKitToggleOpen(kit, add);
    PrLayoutNode popover = prKitPopover(kit, add, {}, {});
    if (popover == kPrLayoutNodeNone)
        return;
    PrLayoutNode menu = prKitMenu(kit, popover, {}, {.minWidth = kMenuWidth});
    if (state.catalog.embeddings.isEmpty())
        prKitMenuItem(kit, menu, {.text = "No embeddings on the server", .disabled = true}, {});
    for (const String& embedding : state.catalog.embeddings) {
        PrKitMenuItem item{.key = prUiKey(kit->ui, menu, embedding), .text = embeddingName(embedding)};
        if (prKitActivated(kit, prKitMenuItem(kit, menu, item, {}))) {
            insertEmbedding(shell, target, embedding);
            prKitSetOpen(kit, add, false);
        }
    }
}

void buildPrompts(PrKit* kit, PrLayoutNode parent, AppState& state, Shell& shell) {
    buildPromptHeader(kit, parent, state, shell, "Prompt", 0);
    shell.promptFields[0] = prKitTextArea(
        kit, parent, {.text = state.params.prompt, .placeholder = "What to make", .rows = 4}, {});
    buildPromptHeader(kit, parent, state, shell, "Negative prompt", 1);
    shell.promptFields[1] = prKitTextArea(
        kit, parent, {.text = state.params.negativePrompt, .placeholder = "What to avoid", .rows = 2}, {});
    PrStringView text{};
    if (prKitTextChanged(kit, shell.promptFields[0], &text))
        state.params.prompt = text;
    if (prKitTextChanged(kit, shell.promptFields[1], &text))
        state.params.negativePrompt = text;
    for (int i = 0; i < 2; ++i) {
        if (prUiIsFocusWithin(shell.ui, shell.promptFields[i]))
            shell.promptTouched[i] = true;
    }
}

void buildLoras(PrKit* kit, PrLayoutNode parent, AppState& state) {
    Array<LoraEntry>& loras = state.params.loras;
    for (uint32_t i = 0; i < loras.count();) {
        // An adapter's row is keyed by its name, so that its controls keep their state as the rows around it change.
        PrKitBoxStyle style{.direction = PrKitDirection_Row, .align = PrKitAlign_Center, .gap = kPrKitSpace3};
        PrLayoutNode line = prKitBox(kit, parent, {.key = prUiKey(kit->ui, parent, loras[i].name)}, style);
        bool conflict = loraConflicts(state, loras[i].name);
        prKitLabel(kit, line, {.text = loras[i].name},
            textStyle(PrKitTextRole_BodySmall, conflict ? PrKitTone_Warning : PrKitTone_Primary));
        prKitSpacer(kit, line, {});
        PrKitNumber weight{
            .value = prKitScalarF64(&loras[i].weight),
            .range = {.min = 0, .max = 2, .hasMin = true, .hasMax = true},
            .precision = 2,
        };
        PrLayoutNode number = prKitNumberField(kit, line, weight, {.size = PrKitControlSize_Sm});
        PrKitPlace narrow{.width = prKitPx(64), .flex = PrKitFlex_Fixed};
        prKitPlace(kit, number, &narrow);
        PrLayoutNode remove = prKitButton(kit, line, {.icon = PrKitIconKind_Close},
            {.variant = PrKitButtonVariant_Ghost, .size = PrKitControlSize_Sm});
        if (prKitActivated(kit, remove)) {
            loras.erase(i);
            continue;
        }
        ++i;
    }
    // The adapters that the add menu offers: those that the checkpoint can load and the chain does not hold.
    Array<String> candidates;
    for (const String& name : state.catalog.loras) {
        bool listed = false;
        for (const LoraEntry& lora : loras)
            listed = listed || lora.name == name;
        if (!listed && !loraConflicts(state, name))
            candidates.push(name);
    }
    String added;
    selectName(kit, row(kit, parent), candidates, added, "No adapters on the server");
    if (!added.isEmpty())
        loras.push({added, 1.0});
}

void buildDimensions(PrKit* kit, PrLayoutNode parent, AppState& state) {
    Dimensions& dims = state.dims;
    PrLayoutNode header = row(kit, parent);
    String family = familyLabel(dims.family);
    if (!dims.detected && !dims.customSize)
        family.append(" (by name)");
    prKitLabel(kit, header, {.text = dims.customSize ? PrStringView("CUSTOM") : family.view()},
        textStyle(PrKitTextRole_Caption, PrKitTone_Muted));
    prKitSpacer(kit, header, {});
    prKitLabel(kit, header, {.text = dimensionLabel(dims.size)}, textStyle(PrKitTextRole_Mono));
    if (prKitActivated(kit, iconButton(kit, header, PrKitIconKind_Swap, "Swap width and height")))
        swapDimensions(dims);
    // The override suspends the ladder for two numbers, which keep the size that the ladder stood on.
    if (prKitActivated(kit, iconButton(kit, header, PrKitIconKind_Scale, "Override the size", dims.customSize)))
        setCustomSize(dims, !dims.customSize);

    if (dims.customSize) {
        PrLayoutNode sizes = row(kit, parent);
        numberField(kit, sizes, prKitScalarU32(&dims.size.width), 64, 4096);
        numberField(kit, sizes, prKitScalarU32(&dims.size.height), 64, 4096);
    } else if (dims.family == DimensionFamily_Flexible) {
        double exponent = dims.aspectExponent;
        prKitSlider(kit, parent, {
            .value = prKitScalarF64(&exponent),
            .min = flexibleAspectMin(),
            .max = flexibleAspectMax(),
        }, {});
        uint32_t count = 0;
        const double* ladder = megapixelLadder(&count);
        PrLayoutNode budgets = prKitSegmented(kit, parent, {}, {.size = PrKitControlSize_Sm});
        uint32_t megapixel = dims.megapixel;
        for (uint32_t i = 0; i < count; ++i) {
            char label[16];
            formatToBuffer(Span(label), "{:g} MP", ladder[i]);
            PrKitChoice choice{.label = label, .selected = i == megapixel};
            if (prKitActivated(kit, prKitSegment(kit, budgets, choice)))
                megapixel = i;
        }
        if (exponent != dims.aspectExponent || megapixel != dims.megapixel)
            selectFlexible(dims, megapixel, exponent);
    } else {
        uint32_t count = 0;
        if (dims.family == DimensionFamily_Sdxl)
            sdxlBuckets(&count);
        else
            sd15Sizes(&count);
        uint32_t preset = dims.family == DimensionFamily_Sdxl ? dims.sdxlPreset : dims.sd15Preset;
        double position = preset;
        prKitSlider(kit, parent, {
            .value = prKitScalarF64(&position),
            .min = 0,
            .max = double(count - 1),
            .step = 1.0f / float(count - 1),
        }, {});
        uint32_t chosen = uint32_t(prF64Round(position));
        if (chosen != preset)
            selectPreset(dims, chosen);
    }
}

void buildSampling(PrKit* kit, PrLayoutNode parent, AppState& state) {
    GenerateParams& p = state.params;
    selectName(kit, captioned(kit, parent, "Checkpoint"), state.catalog.checkpoints, p.checkpoint,
        "No checkpoints on the server", PrKitFlex_Auto);
    prKitLabel(kit, parent, {.text = "Adapters"}, textStyle(PrKitTextRole_Caption, PrKitTone_Muted));
    buildLoras(kit, parent, state);
    PrLayoutNode choices = row(kit, parent, kPrKitSpace3);
    captionedSelect(kit, choices, "Sampler", state.catalog.samplers, p.sampler);
    captionedSelect(kit, choices, "Scheduler", state.catalog.schedulers, p.scheduler);
    PrLayoutNode numbers = row(kit, parent, kPrKitSpace3);
    captionedNumber(kit, numbers, "Steps", prKitScalarU32(&p.steps), 1, 150);
    captionedNumber(kit, numbers, "Guidance", prKitScalarF64(&p.cfg), 1, 30, 1);
    PrLayoutNode seed = field(kit, parent, "Seed");
    numberField(kit, seed, prKitScalarU64(&p.seed), 0, 9007199254740991.0);
    PrKitIconKind lockIcon = p.seedLocked ? PrKitIconKind_Lock : PrKitIconKind_Unlock;
    if (prKitActivated(kit, iconButton(kit, seed, lockIcon, "Keep the seed for the next run", p.seedLocked)))
        p.seedLocked = !p.seedLocked;
}

void buildParameters(PrKit* kit, PrLayoutNode content, AppState& state, Commands& commands) {
    prCommandScope(kit->ui, content, commands.registry, commands.keymap, kContextParameters);
    PrKitBoxStyle scroll{
        .direction = PrKitDirection_Column,
        .overflow = PrKitOverflow_ScrollY,
        .gap = kPrKitSpace4,
        .padding = prKitEdgesAll(kPrKitSpace5),
        .place = {.flex = PrKitFlex_Fill},
    };
    PrLayoutNode panel = prKitBox(kit, content, {}, scroll);
    buildMode(kit, panel, state);
    buildSource(kit, panel, state, commands);
    if (modeSamples(state.mode)) {
        buildPrompts(kit, panel, state, *commands.shell);
        buildSampling(kit, panel, state);
    } else {
        commands.shell->promptFields[0] = commands.shell->promptFields[1] = kPrLayoutNodeNone;
    }
    switch (state.mode) {
        case OperationMode_Generate:
            buildDimensions(kit, panel, state);
            numberField(kit, field(kit, panel, "Batch"), prKitScalarU32(&state.params.batch), 1, 8);
            if (!state.params.sourcePath.isEmpty()) {
                PrLayoutNode strength = field(kit, panel, "Strength");
                prKitSlider(kit, strength, {.value = prKitScalarF64(&state.params.strength), .min = 0.05, .max = 1}, {});
            }
            break;
        case OperationMode_Upscale:
            selectName(kit, field(kit, panel, "Model"), state.catalog.upscaleModels, state.upscale.model,
                "No upscale models on the server");
            numberField(kit, field(kit, panel, "Factor"), prKitScalarF64(&state.upscale.factor), 1, 4, 2);
            break;
        case OperationMode_Outpaint: {
            static const char* const kSides[] = {"Left", "Top", "Right", "Bottom"};
            PrLayoutNode sides = prKitSegmented(kit, field(kit, panel, "Side"), {}, {});
            for (OutpaintDirection side = 0; side < 4; ++side) {
                PrKitChoice choice{.label = kSides[side], .selected = state.outpaint.direction == side};
                if (prKitActivated(kit, prKitSegment(kit, sides, choice)))
                    state.outpaint.direction = side;
            }
            numberField(kit, field(kit, panel, "Amount"), prKitScalarU32(&state.outpaint.amount), 64, 1024);
            numberField(kit, field(kit, panel, "Crossfade"), prKitScalarU32(&state.outpaint.crossfade), 0, 256);
            numberField(kit, field(kit, panel, "Grow mask"), prKitScalarU32(&state.outpaint.growMask), 0, 128);
            numberField(kit, field(kit, panel, "Context"), prKitScalarU32(&state.outpaint.context), 64, 2048);
            break;
        }
        case OperationMode_Detail:
            selectName(kit, field(kit, panel, "Detector"), state.catalog.detectors, state.detail.detector,
                "The server has no detailer nodes");
            break;
    }
}

// The verbs of one operation of the record, which act on that operation by its place in the chain.
struct RecordBlock {
    PrKit* kit;
    PrLayoutNode parent;
    AppState& state;
    uint32_t operation;

    // A caption over a value, with the verb that puts the value's group back into the parameters.
    void value(const char* label, PrStringView text, UseWhat what, const char* verb = "Use") {
        if (text.isEmpty())
            return;
        header(label, what, verb);
        prKitLabel(kit, parent, {.text = text}, wrapped());
    }

    // A prompt as the lines that it was written over: the group's verb replaces the prompt being written, and each
    // line's verb appends that line to it, which is how a prompt is composed out of several images' pieces.
    void prompt(const char* label, PrStringView text, UseWhat what, UseWhat lineWhat) {
        if (text.isEmpty())
            return;
        header(label, what, "Use");
        Array<PrStringView> lines = promptLines(text);
        if (lines.count() < 2) {
            prKitLabel(kit, parent, {.text = text}, wrapped());
            return;
        }
        for (uint32_t i = 0; i < lines.count(); ++i) {
            PrLayoutNode line = row(kit, parent, kPrKitSpace2);
            PrKitBoxStyle fill{.place = {.flex = PrKitFlex_Fill}};
            prKitLabel(kit, prKitBox(kit, line, {}, fill), {.text = lines[i]}, wrapped());
            if (prKitActivated(kit, iconButton(kit, line, PrKitIconKind_Plus, "Append this line to the prompt")))
                useRecorded(state, operation, lineWhat, i);
        }
    }

    void header(const char* label, UseWhat what, const char* verb) {
        PrLayoutNode line = row(kit, parent, kPrKitSpace2);
        prKitLabel(kit, line, {.text = label}, textStyle(PrKitTextRole_Caption, PrKitTone_Muted));
        prKitSpacer(kit, line, {});
        PrLayoutNode use = prKitButton(kit, line, {.text = verb}, {.size = PrKitControlSize_Xs});
        if (prKitActivated(kit, use))
            useRecorded(state, operation, what);
    }

    static PrKitTextStyle wrapped() {
        return PrKitTextStyle{.role = PrKitTextRole_BodySmall, .tone = PrKitTone_Secondary, .wrap = PrKitWrap_Wrap};
    }
};

String fixed(double value, int decimals) {
    return format("{:.{}f}", value, decimals);
}

// The separator between the parts of a line.
constexpr PrStringView kDot = " \xC2\xB7 ";

// Appends `part` to `line`, after `separator` unless the line is empty.
void appendPart(String& line, PrStringView separator, PrStringView part) {
    if (!line.isEmpty())
        line.append(separator);
    line.append(part);
}

// The sampling vocabulary as one line, which is how a run is recognized at a glance.
String samplingLine(const ProvenanceOp& op) {
    String line = op.sampler;
    if (!op.scheduler.isEmpty())
        appendPart(line, " / ", op.scheduler);
    if (op.steps != 0)
        appendPart(line, kDot, format("{} steps", op.steps));
    if (op.cfg != 0)
        appendPart(line, kDot, format("cfg {:.1f}", op.cfg));
    return line;
}

// The adapters that a run loaded, as one line.
String loraLine(const ProvenanceOp& op) {
    String line;
    for (const ProvenanceLora& lora : op.loras)
        appendPart(line, kDot, format("{} {:.2f}", lora.name, lora.weight));
    return line;
}

// What an operation's row states beside its name: the one thing that tells two runs of the same mode apart.
String operationMeta(const ProvenanceOp& op) {
    if (!op.upscaleModel.isEmpty())
        return format("{:.1f}x", op.factor);
    if (!op.direction.isEmpty())
        return format("{} {}px", op.direction, op.amount);
    if (op.width != 0 && op.height != 0)
        return format("{} x {}", op.width, op.height);
    return op.detector;
}

// One operation of the chain: the row that names it, and while it is open every group that it states with the verb that
// applies the group.
void buildOperation(PrKit* kit, PrLayoutNode card, AppState& state, const ProvenanceOp& op, uint32_t operation) {
    PrKitBoxStyle lineStyle{.direction = PrKitDirection_Row, .align = PrKitAlign_Center, .gap = kPrKitSpace2};
    PrLayoutNode line = prKitBox(kit, card, {.key = prUiKeyIndex(kit->ui, card, operation)}, lineStyle);
    bool open = state.recordOpen[operation];
    PrKitIconKind fold = open ? PrKitIconKind_ChevronDown : PrKitIconKind_ChevronRight;
    if (prKitActivated(kit, iconButton(kit, line, fold, open ? "Hide this run" : "Show this run")))
        state.recordOpen[operation] = !open;
    PrStringView mode = op.mode.isEmpty() ? PrStringView("?") : op.mode.view();
    prKitLabel(kit, line, {.text = mode}, textStyle(PrKitTextRole_Body));
    prKitSpacer(kit, line, {});
    prKitLabel(kit, line, {.text = operationMeta(op)}, textStyle(PrKitTextRole_Mono, PrKitTone_Muted));
    if (!open)
        return;

    PrKitBoxStyle bodyStyle{.gap = kPrKitSpace2, .padding = {.left = kPrKitSpace3}};
    PrLayoutNode body = prKitBox(kit, card, {.key = prUiKeyIndex(kit->ui, card, operation + 1000)}, bodyStyle);
    RecordBlock block{kit, body, state, operation};
    block.prompt("Prompt", op.prompt, UseWhat_Prompt, UseWhat_PromptLine);
    block.prompt("Negative", op.negative, UseWhat_Negative, UseWhat_NegativeLine);
    block.value("Model", op.checkpoint, UseWhat_Model);
    block.value("Adapters", loraLine(op), UseWhat_Loras);
    block.value("Sampler", samplingLine(op), UseWhat_Sampling);
    if (op.width != 0 && op.height != 0) {
        block.value("Size", format("{} x {}", op.width, op.height), UseWhat_Aspect, "Use Aspect Ratio");
    }
    if (provenanceSamples(op))
        block.value("Seed", format("{}", op.seed), UseWhat_Seed);
    if (op.strength != 0)
        block.value("Strength", fixed(op.strength, 2), UseWhat_Strength);
    if (!op.upscaleModel.isEmpty())
        block.value("Upscale", format("{} at {:.1f}x", op.upscaleModel, op.factor), UseWhat_Upscale);
    if (!op.direction.isEmpty()) {
        String outpaint = format("{} by {}px, context {}, crossfade {}", op.direction, op.amount, op.context,
            op.crossfade);
        block.value("Outpaint", outpaint, UseWhat_Outpaint);
    }
    block.value("Detector", op.detector, UseWhat_Detector);
    block.value("Source", op.source, UseWhat_Source);
    if (provenanceSamples(op)) {
        PrLayoutNode all = prKitButton(kit, body, {.text = "Use All Settings"}, {.size = PrKitControlSize_Xs});
        if (prKitActivated(kit, all))
            useRecorded(state, operation, UseWhat_All);
    }
}

// The record of the selected image, over the image: every run that made it, newest first, and the verbs that put any
// part of any of them back into the parameters.
void buildRecord(PrKit* kit, PrLayoutNode stack, AppState& state, Commands& commands) {
    const ProvenanceRecord* record = selectedRecord(state);
    PrKitBoxStyle layer{.align = PrKitAlign_End, .padding = prKitEdgesAll(kPrKitSpace5)};
    PrLayoutNode over = prKitLayer(kit, stack, {}, layer);
    PrKitPanelStyle panel{
        .box = {
            .overflow = PrKitOverflow_ScrollY,
            .gap = kPrKitSpace2,
            .padding = prKitEdgesAll(kPrKitSpace3),
            .place = {.width = prKitPx(kRecordWidth), .maxHeight = prKitPx(kRecordMaxHeight), .flex = PrKitFlex_Auto},
        },
        .background = PrKitBackground_Panel,
    };
    PrLayoutNode card = prKitPanel(kit, over, {}, panel);
    PrLayoutNode header = row(kit, card, kPrKitSpace2);
    prKitLabel(kit, header, {.text = "Image Info"}, textStyle(PrKitTextRole_Caption, PrKitTone_Muted));
    if (record != nullptr)
        prKitBadge(kit, header, {.text = format("{}", record->operations.count())}, {});
    prKitSpacer(kit, header, {});
    if (prKitActivated(kit, iconButton(kit, header, PrKitIconKind_Close, "Close")))
        runCommand(commands, "alchemy.toggleInfo");
    if (record == nullptr) {
        prKitLabel(kit, card, {.text = "This image has no record."},
            textStyle(PrKitTextRole_BodySmall, PrKitTone_Faint));
        return;
    }
    if (record->operations.count() > 1)
        prKitLabel(kit, card, {.text = provenanceChain(*record)}, RecordBlock::wrapped());
    for (uint32_t i = record->operations.count(); i > 0; --i)
        buildOperation(kit, card, state, record->operations[i - 1], i - 1);
}

// The menu of what can be done to the selected images, which a thumbnail and the viewport's image both raise.
void buildImageMenu(PrKit* kit, PrLayoutNode host, Commands& commands) {
    PrLayoutNode menu = prKitContextMenu(kit, host, {}, {.minWidth = kMenuWidth});
    if (menu == kPrLayoutNodeNone)
        return;
    const auto item = [&](const char* id, PrKitMenuItemVariant variant = PrKitMenuItemVariant_Default) {
        prCommandMenuItem(kit, menu, commands.registry, commands.keymap, id, {.variant = variant});
    };
    item("alchemy.useAsSource");
    item("alchemy.copyImage");
    prKitMenuSeparator(kit, menu, {});
    item("alchemy.usePrompt");
    item("alchemy.useSeed");
    item("alchemy.useAspect");
    item("alchemy.useAll");
    prKitMenuSeparator(kit, menu, {});
    item("alchemy.deleteImage", PrKitMenuItemVariant_Danger);
    prKitMenuSeparator(kit, menu, {});
    item("alchemy.openGallery");
    item("alchemy.rescan");
}

void buildViewport(PrKit* kit, PrLayoutNode content, AppState& state, Commands& commands) {
    prCommandScope(kit->ui, content, commands.registry, commands.keymap, kContextGallery);
    PrKitPanelStyle bar{
        .box = {
            .direction = PrKitDirection_Row,
            .align = PrKitAlign_Center,
            .gap = kPrKitSpace2,
            .padding = prKitEdgesXY(kPrKitSpace2, kPrKitSpace3),
        },
        .background = PrKitBackground_Header,
    };
    PrLayoutNode toolbar = prKitPanel(kit, content, {}, bar);
    commandButton(kit, toolbar, commands, "alchemy.back", "", PrKitIconKind_ArrowLeft);
    commandButton(kit, toolbar, commands, "alchemy.forward", "", PrKitIconKind_ArrowRight);
    prKitDivider(kit, toolbar, {});
    commandButton(kit, toolbar, commands, "alchemy.useAsSource", "", PrKitIconKind_Image);
    commandButton(kit, toolbar, commands, "alchemy.usePrompt", "", PrKitIconKind_Quote);
    commandButton(kit, toolbar, commands, "alchemy.useSeed", "", PrKitIconKind_Hash);
    commandButton(kit, toolbar, commands, "alchemy.useAspect", "", PrKitIconKind_Ratio);
    commandButton(kit, toolbar, commands, "alchemy.useAll", "", PrKitIconKind_Asterisk);
    prKitDivider(kit, toolbar, {});
    commandButton(kit, toolbar, commands, "alchemy.togglePreview", "", PrKitIconKind_Eye);
    commandButton(kit, toolbar, commands, "alchemy.toggleInfo", "", PrKitIconKind_Info);
    commandButton(kit, toolbar, commands, "alchemy.deleteImage", "", PrKitIconKind_Trash);
    prKitSpacer(kit, toolbar, {});

    const GalleryEntry* entry = gallerySelected(state.gallery);
    const PrDrawImage* image = entry != nullptr ? imageCacheGet(state.images, entry->path, ImageScale_Full) : nullptr;
    const Job* job = activeJob(state);
    bool previewing = state.showPreview && state.preview != nullptr && job != nullptr &&
        job->promptId == state.previewPromptId;
    if (entry != nullptr) {
        String label = entry->filename;
        if (image != nullptr) {
            PrUVec2 extent = prDrawImageExtent(image);
            formatTo(label, "  {} x {}", extent.x, extent.y);
        }
        prKitLabel(kit, toolbar, {.text = label}, textStyle(PrKitTextRole_Mono, PrKitTone_Muted));
    }

    PrKitPanelStyle background{
        .box = {
            .justify = PrKitJustify_Center,
            .align = PrKitAlign_Center,
            .overflow = PrKitOverflow_Clip,
            .padding = prKitEdgesAll(kPrKitSpace5),
            .place = {.flex = PrKitFlex_Fill},
        },
        .background = PrKitBackground_Viewport,
    };
    PrLayoutNode stage = prKitPanel(kit, content, {}, background);
    PrLayoutNode stack = prKitStack(kit, stage, {}, {
        .justify = PrKitJustify_Center,
        .align = PrKitAlign_Center,
        .place = {.width = prKitPercent(100), .height = prKitPercent(100)},
    });
    const PrDrawImage* shown = previewing ? state.preview : image;
    PrKitImageStyle fit{
        .fit = PrKitImageFit(previewing ? PrKitImageFit_Contain : PrKitImageFit_ScaleDown),
        .place = {.width = prKitPercent(100), .height = prKitPercent(100)},
    };
    if (shown != nullptr) {
        prKitImage(kit, stack, {.image = shown}, fit);
    } else if (entry == nullptr) {
        prKitEmptyState(kit, stack, {
            .icon = PrKitIconKind_Image,
            .title = "No images yet",
            .hint = "Connect to a ComfyUI server and generate one.",
        });
    } else if (imageCacheFailed(state.images, entry->path, ImageScale_Full)) {
        prKitEmptyState(kit, stack, {.icon = PrKitIconKind_Alert, .title = "This image cannot be read"});
    }
    if (state.showInfo && entry != nullptr && !previewing)
        buildRecord(kit, stack, state, commands);
    if (job != nullptr) {
        PrKitBoxStyle bottom{.justify = PrKitJustify_End, .padding = prKitEdgesAll(kPrKitSpace4)};
        PrLayoutNode over = prKitLayer(kit, stack, {}, bottom);
        float done = job->progressMax != 0 ? float(job->progressValue) / float(job->progressMax) : 0.0f;
        prKitProgress(kit, over, {.value = done, .indeterminate = job->progressMax == 0});
    }
    buildImageMenu(kit, stage, commands);
}

void buildBrowser(PrKit* kit, PrLayoutNode content, AppState& state, Commands& commands) {
    prCommandScope(kit->ui, content, commands.registry, commands.keymap, kContextGallery);
    Gallery& gallery = state.gallery;
    if (gallery.entries.isEmpty()) {
        prKitEmptyState(kit, content, {
            .icon = PrKitIconKind_Folder,
            .title = "The gallery is empty",
            .hint = gallery.dir,
        });
        return;
    }
    PrUi* ui = kit->ui;
    PrKitListViewStyle style{.rowHeight = kTileHeight, .tileWidth = kTileWidth, .place = {.flex = PrKitFlex_Fill}};
    PrKitListView list{.count = gallery.entries.count(), .selection = &gallery.selection};
    PrLayoutNode view = prKitListView(kit, content, list, style);
    PrKitWindow window = prKitListWindow(kit, view);
    uint32_t pendingSelect = kPrKitRowNone;
    for (uint32_t i = window.first; i < window.first + window.count && i < gallery.entries.count(); ++i) {
        const GalleryEntry& entry = gallery.entries[i];
        PrKitTile item{.index = i, .item = hashKey(entry.filename), .selected = gallery.selected[i] != 0};
        PrLayoutNode tile = prKitListTile(kit, view, item);
        // The thumbnail is an icon until the cache has decoded the image.
        const PrDrawImage* image = imageCacheGet(state.images, entry.path, ImageScale_Thumbnail);
        if (image != nullptr)
            prKitTileImage(kit, tile, {.image = image}, {.hoverZoom = kTileHoverZoom});
        else
            prKitTileIcon(kit, tile, {.icon = PrKitIconKind_Image}, PrColor{0, 0, 0, 0});
        prKitTileLabel(kit, tile, {.text = entry.filename});
        // A secondary click acts on what it lands on: a press outside the selection replaces it, and a press inside a
        // selection of several leaves all of them for the menu to act on.
        PrVec2 pointer{};
        if (prUiClicked(ui, tile, 1) && prUiPointer(ui, nullptr, &pointer)) {
            if (gallery.selected[i] == 0)
                pendingSelect = i;
            prKitOpenContextMenu(kit, view, pointer);
        }
    }
    PrKitSelectionEdit edit{};
    if (prKitSelectionEdited(kit, view, &edit)) {
        prKitSelectionApply(&edit, gallery.selected.data(), gallery.entries.count());
        gallery.lead = gallery.selection.lead;
    }
    if (pendingSelect != kPrKitRowNone)
        gallerySelect(gallery, pendingSelect);
    buildImageMenu(kit, view, commands);
}

void buildMenus(PrKit* kit, PrLayoutNode bar, Commands& commands) {
    // Each menu's command ids, with null for a separator.
    static const char* const kAlchemyItems[] = {
        "alchemy.configureServer", "alchemy.connect", "alchemy.disconnect", nullptr, "view.palette", nullptr, "app.quit",
    };
    static const char* const kRunItems[] = {
        "alchemy.generate", "alchemy.interrupt", nullptr, "alchemy.mode.generate", "alchemy.mode.upscale",
        "alchemy.mode.outpaint", "alchemy.mode.detail", nullptr, "alchemy.newSeed", "alchemy.lockSeed",
        "alchemy.swapDimensions", "alchemy.clearSource",
    };
    static const char* const kImageItems[] = {
        "alchemy.back", "alchemy.forward", "alchemy.previousImage", "alchemy.nextImage", nullptr, "alchemy.useAsSource",
        "alchemy.usePrompt", "alchemy.useSeed", "alchemy.useAspect", "alchemy.useAll", nullptr, "alchemy.copyImage",
        "alchemy.pasteImage", "alchemy.importImage", nullptr, "alchemy.deleteImage", "alchemy.rescan",
        "alchemy.openGallery",
    };
    static const char* const kViewItems[] = {"alchemy.toggleInfo", "alchemy.togglePreview", nullptr, "view.fullscreen"};
    struct Menu {
        const char* title;
        Span<const char* const> items;
    };
    const Menu kMenus[] = {
        {"Alchemy", Span(kAlchemyItems)},
        {"Run", Span(kRunItems)},
        {"Image", Span(kImageItems)},
        {"View", Span(kViewItems)},
    };
    for (const Menu& menu : kMenus) {
        PrLayoutNode item = prKitMenuBarItem(kit, bar, {.text = menu.title});
        PrLayoutNode popover = prKitPopover(kit, item, {}, {});
        if (popover == kPrLayoutNodeNone)
            continue;
        PrLayoutNode list = prKitMenu(kit, popover, {}, {.minWidth = 240});
        for (const char* id : menu.items) {
            if (id == nullptr)
                prKitMenuSeparator(kit, list, {});
            else
                prCommandMenuItem(kit, list, commands.registry, commands.keymap, id, {});
        }
    }
}

// Creates the dialog of `trigger`, which is open while `open` is true. `shown` holds whether the dialog was told so. A
// dismissal of the dialog clears `open`. Returns the dialog's body, or none while it is closed.
PrLayoutNode requestedDialog(PrKit* kit, PrLayoutNode trigger, bool& open, bool& shown, PrKitDialog dialog,
        PrKitDialogStyle style = {}) {
    if (open != shown) {
        prKitSetOpen(kit, trigger, open);
        shown = open;
    }
    PrLayoutNode body = prKitDialog(kit, trigger, dialog, style);
    if (body == kPrLayoutNodeNone)
        open = shown = false;
    return body;
}

// The address of the server, and the connection to it.
void buildServerDialog(PrKit* kit, PrLayoutNode trigger, AppState& state, View& view) {
    PrLayoutNode body = requestedDialog(kit, trigger, state.serverOpen, view.serverShown, {.title = "Server"});
    if (body == kPrLayoutNodeNone)
        return;
    PrLayoutNode address = captioned(kit, body, "Address");
    PrStringView text{};
    PrKitTextField server{.text = state.server, .placeholder = "http://127.0.0.1:8188"};
    if (prKitTextChanged(kit, prKitTextField(kit, address, server, {}), &text))
        state.server = text;
    if (!state.status.isEmpty()) {
        prKitLabel(kit, body, {.text = state.status},
            PrKitTextStyle{.role = PrKitTextRole_BodySmall, .tone = PrKitTone_Muted, .wrap = PrKitWrap_Wrap});
    }
    PrLayoutNode footer = prKitDialogFooter(kit, body);
    // Connecting to a changed address replaces the connection, so Disconnect shows only for the address in use.
    bool holding = state.connection != ConnectionState_Disconnected && state.server == state.connectedServer;
    PrKitButtonVariant variant = holding ? PrKitButtonVariant_Default : PrKitButtonVariant_Primary;
    PrKitButton button{.text = holding ? "Disconnect" : "Connect", .icon = PrKitIconKind_Link};
    PrLayoutNode connection = prKitButton(kit, footer, button, {.variant = variant});
    if (prKitActivated(kit, connection)) {
        if (holding)
            disconnectServer(state);
        else
            connectServer(state);
    }
    if (prKitActivated(kit, prKitButton(kit, footer, {.text = "Close"}, {})))
        state.serverOpen = false;
}

// What a paste brought, as it will land in the gallery. Paste reads the clipboard, and nothing is imported until the
// preview has been seen and confirmed.
void buildImportDialog(PrKit* kit, PrLayoutNode trigger, AppState& state, View& view, Commands& commands) {
    PrLayoutNode body = requestedDialog(kit, trigger, state.import.open, view.importShown, {.title = "Import Image"},
        {.width = kImportPreview + 2 * kPrKitSpace5});
    if (body == kPrLayoutNodeNone) {
        closeImport(state);
        return;
    }
    prCommandScope(kit->ui, body, commands.registry, commands.keymap, kContextImport);
    PrKitPanelStyle stage{
        .box = {
            .justify = PrKitJustify_Center,
            .align = PrKitAlign_Center,
            .overflow = PrKitOverflow_Clip,
            .place = {.height = prKitPx(kImportPreview), .flex = PrKitFlex_Fixed},
        },
        .background = PrKitBackground_Viewport,
    };
    PrLayoutNode preview = prKitPanel(kit, body, {}, stage);
    const PrDrawImage* image = state.import.image;
    if (image != nullptr) {
        PrKitImageStyle fit{
            .fit = PrKitImageFit_ScaleDown,
            .place = {.width = prKitPercent(100), .height = prKitPercent(100)},
        };
        prKitImage(kit, preview, {.image = image}, fit);
    } else {
        PrRawString chord = prCommandChordText(PrCommandChord{.key = 'V', .modifiers = PrAppModifierFlag_Super});
        String hint = format("Press {} to read the clipboard.", prRawStringView(&chord));
        prRawStringReset(&chord);
        prKitEmptyState(kit, preview, {.icon = PrKitIconKind_Image, .title = "Nothing to import yet", .hint = hint});
    }
    String caption = state.import.message;
    if (caption.isEmpty() && image != nullptr) {
        PrUVec2 extent = prDrawImageExtent(image);
        caption = format("{} x {}", extent.x, extent.y);
    }
    if (!caption.isEmpty()) {
        PrKitTone tone = state.import.message.isEmpty() ? PrKitTone_Muted : PrKitTone_Warning;
        prKitLabel(kit, body, {.text = caption}, textStyle(PrKitTextRole_BodySmall, tone));
    }
    PrLayoutNode footer = prKitDialogFooter(kit, body);
    PrLayoutNode paste = commandButton(kit, footer, commands, "alchemy.pasteImage", "Paste", PrKitIconKind_Image,
        PrKitButtonVariant_Default, PrKitControlSize_Md);
    // The dialog's chord applies while focus is in its body, which the first focusable of the dialog is not.
    prKitModalAutofocus(kit, paste);
    prKitSpacer(kit, footer, {});
    if (prKitActivated(kit, prKitButton(kit, footer, {.text = "Cancel"}, {})))
        closeImport(state);
    PrLayoutNode confirm = prKitButton(kit, footer, {.text = "Import", .disabled = image == nullptr},
        {.variant = PrKitButtonVariant_Primary});
    if (prKitActivated(kit, confirm))
        confirmImport(state);
}

void buildDeleteDialog(PrKit* kit, PrLayoutNode trigger, AppState& state, View& view) {
    PrLayoutNode body = requestedDialog(kit, trigger, state.deleteOpen, view.deleteShown, {.title = "Delete images"});
    if (body == kPrLayoutNodeNone)
        return;
    String question;
    if (state.deletePaths.count() == 1)
        question = format("Delete {}?", prFilePathName(state.deletePaths[0]));
    else
        question = format("Delete {} images?", state.deletePaths.count());
    prKitLabel(kit, body, {.text = question}, textStyle(PrKitTextRole_Body));
    prKitLabel(kit, body, {.text = "The files are removed from the gallery's folder, which cannot be undone."},
        PrKitTextStyle{.role = PrKitTextRole_BodySmall, .tone = PrKitTone_Muted, .wrap = PrKitWrap_Wrap});
    PrLayoutNode footer = prKitDialogFooter(kit, body);
    if (prKitActivated(kit, prKitButton(kit, footer, {.text = "Cancel"}, {})))
        state.deleteOpen = false;
    PrLayoutNode confirm = prKitButton(kit, footer, {.text = "Delete"}, {.variant = PrKitButtonVariant_Danger});
    if (prKitActivated(kit, confirm))
        confirmDelete(state);
}

} // namespace

void viewInit(View& view) {
    view.dock = prKitDockCreate();
    PrKitDockNode center = prKitDockPrimary(view.dock);
    prKitDockAddPanel(view.dock, center, Panel_Viewport);
    prKitDockSplit(view.dock, center, PrKitDockEdge_Left, Panel_Parameters, 0.28f);
    prKitDockSplit(view.dock, center, PrKitDockEdge_Right, Panel_Browser, 0.3f);
}

void viewReset(View& view) {
    prKitDockDestroy(view.dock);
    view.dock = nullptr;
}

// The title bar, in three parts: the menus at the start, the run of the current mode and the stop in the middle, and
// what the server is doing and the connection to it at the end. The outer parts share the room equally, so that the run
// stays centered. The stop keeps its place whether or not a run is in flight, so that nothing moves under the pointer
// when one starts.
void buildTitleBarRow(PrKit* kit, PrLayoutNode bar, View& view, Commands& commands) {
    static const char* const kRuns[] = {"Generate", "Upscale", "Outpaint", "Detail"};
    AppState& state = *commands.shell->state;
    PrKitBoxStyle side{
        .direction = PrKitDirection_Row,
        .align = PrKitAlign_Center,
        .overflow = PrKitOverflow_Clip,
        .gap = kPrKitSpace4,
        .place = {.flex = PrKitFlex_Fill},
    };
    PrLayoutNode start = prKitBox(kit, bar, {}, side);
    PrLayoutNode menus = prKitMenuBar(kit, start, {.plain = true});
    buildMenus(kit, menus, commands);
    if (commands.shell->paletteRequested) {
        prKitSetOpen(kit, menus, true);
        commands.shell->paletteRequested = false;
    }
    PrLayoutNode list = prCommandPalette(kit, menus, commands.registry, commands.keymap, view.paletteQuery);
    PrStringView query{};
    if (list == kPrLayoutNodeNone)
        view.paletteQuery.clear();
    else if (prKitTextChanged(kit, list, &query))
        view.paletteQuery = query;

    PrLayoutNode run = row(kit, bar);
    PrKitPlace fixed{.flex = PrKitFlex_Fixed};
    prKitPlace(kit, run, &fixed);
    commandButton(kit, run, commands, "alchemy.generate", kRuns[state.mode], PrKitIconKind_Play,
        PrKitButtonVariant_Primary, PrKitControlSize_Sm);
    commandButton(kit, run, commands, "alchemy.interrupt", "", PrKitIconKind_Stop, PrKitButtonVariant_Danger,
        PrKitControlSize_Sm);

    // The activity: the active job's stage and progress, or the last message while nothing runs.
    side.justify = PrKitJustify_End;
    PrLayoutNode end = prKitBox(kit, bar, {}, side);
    if (const Job* job = activeJob(state)) {
        String stage = job->node.isEmpty() ? String("queued") : job->node;
        prKitLabel(kit, end, {.text = stage}, textStyle(PrKitTextRole_Mono, PrKitTone_Accent));
        if (job->progressMax != 0) {
            float done = float(job->progressValue) / float(job->progressMax);
            PrLayoutNode progress = prKitProgress(kit, end, {.value = done});
            PrKitPlace width{.width = prKitPx(120)};
            prKitPlace(kit, progress, &width);
            String steps = format("{}/{}", job->progressValue, job->progressMax);
            prKitLabel(kit, end, {.text = steps}, textStyle(PrKitTextRole_Mono, PrKitTone_Muted));
        }
        uint32_t inFlight = runsInFlight(state);
        if (inFlight > 1 || state.queueRemaining > 1) {
            String queued = format("{} queued", max(inFlight, state.queueRemaining));
            prKitLabel(kit, end, {.text = queued}, textStyle(PrKitTextRole_Mono, PrKitTone_Muted));
        }
    } else {
        prKitLabel(kit, end, {.text = state.status}, textStyle(PrKitTextRole_BodySmall, PrKitTone_Muted));
    }

    // The connection's state, whose server opens the server's dialog.
    static const char* const kStates[] = {"Disconnected", "Connecting", "Connected"};
    PrKitTone tones[] = {PrKitTone_Muted, PrKitTone_Warning, PrKitTone_Success};
    PrKitTextStyle connection = textStyle(PrKitTextRole_Caption, tones[state.connection]);
    prKitLabel(kit, end, {.text = kStates[state.connection]}, connection);
    view.server = prKitButton(kit, end, {.text = state.server, .icon = PrKitIconKind_Link},
        {.variant = PrKitButtonVariant_Ghost, .size = PrKitControlSize_Xs});
    prKitTooltip(kit, view.server, {.text = "Configure the server"});
    if (prKitActivated(kit, view.server))
        state.serverOpen = true;
}

void buildView(PrKit* kit, PrLayoutNode root, View& view, Commands& commands) {
    AppState& state = *commands.shell->state;
    prCommandScope(kit->ui, root, commands.registry, commands.keymap, "");
    view.browserTitle = format("Browser · {}", state.gallery.entries.count());
    PrLayoutNode dock = prKitDockBuild(kit, root, {.dock = view.dock, .describe = describePanel, .context = &view});
    PrKitPlace fill{.flex = PrKitFlex_Fill};
    prKitPlace(kit, dock, &fill);
    for (uint32_t i = 0; i < prKitDockPaneCount(view.dock); ++i) {
        PrKitDockPane pane = prKitDockPane(view.dock, i);
        switch (pane.panel) {
            case Panel_Parameters: buildParameters(kit, pane.content, state, commands); break;
            case Panel_Browser: buildBrowser(kit, pane.content, state, commands); break;
            default: buildViewport(kit, pane.content, state, commands); break;
        }
    }
    if (view.server != kPrLayoutNodeNone)
        buildServerDialog(kit, view.server, state, view);
    buildDeleteDialog(kit, dock, state, view);
    // The import's dialog has no control of its own to float from, since a command or a chord opens it.
    PrLayoutNode importTrigger = prKitBox(kit, root, {}, {});
    buildImportDialog(kit, importTrigger, state, view, commands);
}

} // namespace alchemy
