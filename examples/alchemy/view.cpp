// Alchemy's interface.

#include "examples/alchemy/view.h"

#include "pr/command_kit.h"
#include "pr/kit_box.h"
#include "pr/kit_button.h"
#include "pr/kit_choice.h"
#include "pr/kit_collection.h"
#include "pr/kit_image.h"
#include "pr/kit_input.h"
#include "pr/kit_menu.h"
#include "pr/kit_surface.h"
#include "pr/kit_text.h"

#include "examples/alchemy/text.h"

#include <cmath>
#include <memory>

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

PrKitDockTab describePanel(void* PR_NULLABLE, PrKitDockPanelId panel) {
    switch (panel) {
        case Panel_Parameters: return {.title = "Parameters"};
        case Panel_Browser: return {.title = "Browser"};
        default: return {.title = "Viewport"};
    }
}

PrUiTextBuffer bufferOf(std::string& owned) {
    PrUiTextBuffer buffer{};
    buffer.context = &owned;
    buffer.read = [](void* PR_NULLABLE context) -> PrStringView {
        return *static_cast<const std::string*>(context);
    };
    buffer.replace = [](void* PR_NULLABLE context, uint32_t start, uint32_t end, PrStringView value) {
        static_cast<std::string*>(context)->replace(start, end - start, value.data, value.size);
    };
    return buffer;
}

PrKitTextStyle textStyle(PrKitTextRole role, PrKitTone tone = PrKitTone_Auto) {
    return PrKitTextStyle{.role = role, .tone = tone, .wrap = PrKitWrap_Clip};
}

PrLayoutNode row(PrKit* kit, PrLayoutNode parent, float gap = kPrKitSpace3) {
    PrKitBoxStyle style{.direction = PrKitDirection_Row, .align = PrKitAlign_Center, .gap = gap};
    return prKitBox(kit, parent, {}, style);
}

// A button that runs the command `id`, disabled while the command is.
void commandButton(PrKit* kit, PrLayoutNode parent, Commands& commands, PrStringView id, PrStringView text,
        PrKitIconKind icon, PrKitButtonVariant variant = PrKitButtonVariant_Ghost) {
    PrCommandIndex index = prCommandRegistryFind(commands.registry, id);
    bool disabled = index == kPrCommandMissing ||
        (prCommandRegistryState(commands.registry, index) & PrCommandStateFlag_Disabled) != 0;
    PrKitButton button{.key = prUiKey(prKitUi(kit), parent, id), .text = text, .icon = icon, .disabled = disabled};
    PrLayoutNode node = prKitButton(kit, parent, button, {.variant = variant, .size = PrKitControlSize_Sm});
    if (!text.size && index != kPrCommandMissing)
        prKitTooltip(kit, node, {.text = prCommandRegistryCommand(commands.registry, index).title});
    if (prKitActivated(kit, node))
        runCommand(commands, id);
}

// A select of `names` that writes the chosen name to `current`. An empty list shows `missing`.
void selectName(PrKit* kit, PrLayoutNode parent, const std::vector<std::string>& names, std::string& current,
        PrStringView missing) {
    PrStringView label = names.empty() ? missing : current.empty() ? PrStringView("Choose...") : current;
    PrLayoutNode select = prKitSelect(kit, parent, {.label = label, .disabled = names.empty()}, {});
    PrKitPlace fill{.flex = PrKitFlex_Fill};
    prKitPlace(kit, select, &fill);
    int32_t step = 0;
    if (prKitStepped(kit, select, &step) && !names.empty()) {
        std::vector<std::string>::const_iterator at = std::find(names.begin(), names.end(), current);
        int64_t index = at == names.end() ? 0 : int64_t(at - names.begin()) + step;
        current = names[size_t(std::clamp<int64_t>(index, 0, int64_t(names.size()) - 1))];
    }
    PrLayoutNode menu = prKitSelectMenu(kit, select);
    if (menu == kPrLayoutNodeNone)
        return;
    for (size_t i = 0; i < names.size(); ++i) {
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

void buildServer(PrKit* kit, PrLayoutNode parent, AppState& state, Commands& commands) {
    PrLayoutNode line = field(kit, parent, "Server");
    bool connected = state.connection == ConnectionState_Connected;
    prKitTextField(kit, line, {.text = bufferOf(state.server), .placeholder = "http://127.0.0.1:8188"}, {});
    commandButton(kit, line, commands, connected ? "alchemy.disconnect" : "alchemy.connect",
        connected ? "Disconnect" : "Connect", PrKitIconKind_Link, PrKitButtonVariant_Default);
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

void buildSource(PrKit* kit, PrLayoutNode parent, AppState& state, Commands& commands) {
    PrLayoutNode line = field(kit, parent, "Source");
    bool held = !state.params.sourcePath.empty();
    PrStringView name = held ? pathFilename(state.params.sourcePath) : PrStringView("none");
    PrKitTone tone = held ? PrKitTone_Primary : PrKitTone_Faint;
    prKitLabel(kit, line, {.text = name}, textStyle(PrKitTextRole_Mono, tone));
    prKitSpacer(kit, line, {});
    commandButton(kit, line, commands, "alchemy.useAsSource", "", PrKitIconKind_Target);
    commandButton(kit, line, commands, "alchemy.clearSource", "", PrKitIconKind_Close);
}

void buildPrompts(PrKit* kit, PrLayoutNode parent, AppState& state, Shell& shell) {
    prKitLabel(kit, parent, {.text = "Prompt"}, textStyle(PrKitTextRole_Caption, PrKitTone_Muted));
    shell.promptFields[0] = prKitTextArea(
        kit, parent, {.text = bufferOf(state.params.prompt), .placeholder = "What to make", .rows = 4}, {});
    prKitLabel(kit, parent, {.text = "Negative prompt"}, textStyle(PrKitTextRole_Caption, PrKitTone_Muted));
    shell.promptFields[1] = prKitTextArea(
        kit, parent, {.text = bufferOf(state.params.negativePrompt), .placeholder = "What to avoid", .rows = 2}, {});
}

void buildLoras(PrKit* kit, PrLayoutNode parent, AppState& state) {
    std::vector<LoraEntry>& loras = state.params.loras;
    for (size_t i = 0; i < loras.size();) {
        // An adapter's row is keyed by its name, so that its controls keep their state as the rows around it change.
        PrKitBoxStyle style{.direction = PrKitDirection_Row, .align = PrKitAlign_Center, .gap = kPrKitSpace3};
        PrLayoutNode line = prKitBox(kit, parent, {.key = prUiKey(prKitUi(kit), parent, loras[i].name)}, style);
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
            loras.erase(loras.begin() + long(i));
            continue;
        }
        ++i;
    }
    // The adapters that the add menu offers: those that the checkpoint can load and the chain does not hold.
    std::vector<std::string> candidates;
    for (const std::string& name : state.catalog.loras) {
        bool listed = std::find_if(loras.begin(), loras.end(), [&](const LoraEntry& lora) { return lora.name == name; }) !=
            loras.end();
        if (!listed && !loraConflicts(state, name))
            candidates.push_back(name);
    }
    std::string added;
    selectName(kit, row(kit, parent), candidates, added, "No adapters on the server");
    if (!added.empty())
        loras.push_back({added, 1.0});
}

void buildDimensions(PrKit* kit, PrLayoutNode parent, AppState& state) {
    Dimensions& dims = state.dims;
    PrLayoutNode header = row(kit, parent);
    std::string family = stringOf(familyLabel(dims.family));
    if (!dims.detected && !dims.customSize)
        family += " (by name)";
    prKitLabel(kit, header, {.text = dims.customSize ? std::string("CUSTOM") : family},
        textStyle(PrKitTextRole_Caption, PrKitTone_Muted));
    prKitSpacer(kit, header, {});
    prKitLabel(kit, header, {.text = dimensionLabel(dims.size)}, textStyle(PrKitTextRole_Mono));
    PrLayoutNode swap = prKitButton(kit, header, {.icon = PrKitIconKind_Swap},
        {.variant = PrKitButtonVariant_Ghost, .size = PrKitControlSize_Sm});
    if (prKitActivated(kit, swap))
        swapDimensions(dims);

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
            snprintf(label, sizeof(label), "%g MP", ladder[i]);
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
        uint32_t chosen = uint32_t(std::lround(position));
        if (chosen != preset)
            selectPreset(dims, chosen);
    }
    bool custom = dims.customSize;
    prKitToggle(kit, parent, {.value = &custom, .label = "Size override"}, {});
    if (custom != dims.customSize)
        setCustomSize(dims, custom);
}

void buildSampling(PrKit* kit, PrLayoutNode parent, AppState& state) {
    GenerateParams& p = state.params;
    selectName(kit, field(kit, parent, "Checkpoint"), state.catalog.checkpoints, p.checkpoint, "No checkpoints on the server");
    prKitLabel(kit, parent, {.text = "Adapters"}, textStyle(PrKitTextRole_Caption, PrKitTone_Muted));
    buildLoras(kit, parent, state);
    selectName(kit, field(kit, parent, "Sampler"), state.catalog.samplers, p.sampler, "None");
    selectName(kit, field(kit, parent, "Scheduler"), state.catalog.schedulers, p.scheduler, "None");
    numberField(kit, field(kit, parent, "Steps"), prKitScalarU32(&p.steps), 1, 150);
    numberField(kit, field(kit, parent, "Guidance"), prKitScalarF64(&p.cfg), 1, 30, 1);
    PrLayoutNode seed = field(kit, parent, "Seed");
    numberField(kit, seed, prKitScalarU64(&p.seed), 0, 9007199254740991.0);
    PrKitIconKind lockIcon = p.seedLocked ? PrKitIconKind_Lock : PrKitIconKind_Unlock;
    PrLayoutNode lock = prKitButton(kit, seed, {.icon = lockIcon, .selected = p.seedLocked},
        {.variant = PrKitButtonVariant_Ghost, .size = PrKitControlSize_Sm});
    if (prKitActivated(kit, lock))
        p.seedLocked = !p.seedLocked;
}

void buildParameters(PrKit* kit, PrLayoutNode content, AppState& state, Commands& commands) {
    prCommandScope(prKitUi(kit), content, commands.registry, commands.keymap, kContextParameters);
    PrKitBoxStyle scroll{
        .direction = PrKitDirection_Column,
        .overflow = PrKitOverflow_ScrollY,
        .gap = kPrKitSpace4,
        .padding = prKitEdgesAll(kPrKitSpace5),
        .place = {.flex = PrKitFlex_Fill},
    };
    PrLayoutNode panel = prKitBox(kit, content, {}, scroll);
    buildServer(kit, panel, state, commands);
    buildMode(kit, panel, state);
    buildSource(kit, panel, state, commands);
    if (modeSamples(state.mode)) {
        buildPrompts(kit, panel, state, *commands.shell);
        buildSampling(kit, panel, state);
    }
    switch (state.mode) {
        case OperationMode_Generate:
            buildDimensions(kit, panel, state);
            numberField(kit, field(kit, panel, "Batch"), prKitScalarU32(&state.params.batch), 1, 8);
            if (!state.params.sourcePath.empty()) {
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
    PrLayoutNode actions = row(kit, panel);
    commandButton(kit, actions, commands, "alchemy.generate", "Generate", PrKitIconKind_Play,
        PrKitButtonVariant_Primary);
    commandButton(kit, actions, commands, "alchemy.interrupt", "Stop", PrKitIconKind_Stop, PrKitButtonVariant_Default);
}

// The record of the selected image, over the viewport.
void buildRecord(PrKit* kit, PrLayoutNode stack, AppState& state) {
    const ProvenanceRecord* record = selectedRecord(state);
    PrKitBoxStyle layer{.justify = PrKitJustify_End, .padding = prKitEdgesAll(kPrKitSpace5)};
    PrLayoutNode over = prKitLayer(kit, stack, {}, layer);
    PrKitSurfaceStyle chip{
        .box = {.direction = PrKitDirection_Column, .gap = kPrKitSpace2, .padding = prKitEdgesAll(kPrKitSpace4)},
        .ground = PrKitGround_Hud,
    };
    PrLayoutNode card = prKitSurface(kit, over, {}, chip);
    if (record == nullptr) {
        prKitLabel(kit, card, {.text = "This image carries no record."}, textStyle(PrKitTextRole_BodySmall));
        return;
    }
    prKitLabel(kit, card, {.text = provenanceChain(*record)}, textStyle(PrKitTextRole_Caption));
    const ProvenanceOp* op = provenanceNewest(*record);
    PrKitTextStyle wrapped{.role = PrKitTextRole_BodySmall, .wrap = PrKitWrap_Wrap};
    if (!op->prompt.empty())
        prKitLabel(kit, card, {.text = op->prompt}, wrapped);
    std::string details;
    if (!op->checkpoint.empty())
        details += op->checkpoint + "  ";
    if (provenanceSamples(*op))
        details += op->sampler + "/" + op->scheduler + "  " + std::to_string(op->steps) + " steps  seed " +
            std::to_string(op->seed);
    if (op->width != 0)
        details += "  " + std::to_string(op->width) + "x" + std::to_string(op->height);
    if (!op->upscaleModel.empty())
        details += op->upscaleModel;
    prKitLabel(kit, card, {.text = details}, textStyle(PrKitTextRole_Mono, PrKitTone_Muted));
}

void buildViewport(PrKit* kit, PrLayoutNode content, AppState& state, Commands& commands) {
    prCommandScope(prKitUi(kit), content, commands.registry, commands.keymap, kContextGallery);
    PrKitSurfaceStyle bar{
        .box = {
            .direction = PrKitDirection_Row,
            .align = PrKitAlign_Center,
            .gap = kPrKitSpace2,
            .padding = prKitEdgesXY(kPrKitSpace2, kPrKitSpace3),
        },
        .ground = PrKitGround_Header,
    };
    PrLayoutNode toolbar = prKitSurface(kit, content, {}, bar);
    commandButton(kit, toolbar, commands, "alchemy.back", "", PrKitIconKind_ArrowLeft);
    commandButton(kit, toolbar, commands, "alchemy.forward", "", PrKitIconKind_ArrowRight);
    commandButton(kit, toolbar, commands, "alchemy.useAsSource", "", PrKitIconKind_Target);
    commandButton(kit, toolbar, commands, "alchemy.usePrompt", "", PrKitIconKind_Quote);
    commandButton(kit, toolbar, commands, "alchemy.useSeed", "", PrKitIconKind_Hash);
    commandButton(kit, toolbar, commands, "alchemy.useAspect", "", PrKitIconKind_Ratio);
    commandButton(kit, toolbar, commands, "alchemy.useAll", "", PrKitIconKind_Asterisk);
    commandButton(kit, toolbar, commands, "alchemy.toggleInfo", "", PrKitIconKind_Info);
    commandButton(kit, toolbar, commands, "alchemy.deleteImage", "", PrKitIconKind_Trash);
    prKitSpacer(kit, toolbar, {});

    const GalleryEntry* entry = gallerySelected(state.gallery);
    const PrDrawImage* image = entry != nullptr ? imageCacheGet(state.images, entry->path, ImageScale_Full) : nullptr;
    const Job* job = activeJob(state);
    bool previewing = state.showPreview && state.preview != nullptr && job != nullptr &&
        job->promptId == state.previewPromptId;
    if (entry != nullptr) {
        std::string label = entry->filename;
        if (image != nullptr) {
            PrUVec2 extent = prDrawImageExtent(image);
            label += "  " + std::to_string(extent.x) + " x " + std::to_string(extent.y);
        }
        prKitLabel(kit, toolbar, {.text = label}, textStyle(PrKitTextRole_Mono, PrKitTone_Muted));
    }

    PrKitSurfaceStyle ground{
        .box = {
            .justify = PrKitJustify_Center,
            .align = PrKitAlign_Center,
            .overflow = PrKitOverflow_Clip,
            .padding = prKitEdgesAll(kPrKitSpace5),
            .place = {.flex = PrKitFlex_Fill},
        },
        .ground = PrKitGround_Viewport,
    };
    PrLayoutNode stage = prKitSurface(kit, content, {}, ground);
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
        buildRecord(kit, stack, state);
    if (job != nullptr) {
        PrKitBoxStyle bottom{.justify = PrKitJustify_End, .padding = prKitEdgesAll(kPrKitSpace4)};
        PrLayoutNode over = prKitLayer(kit, stack, {}, bottom);
        float done = job->progressMax != 0 ? float(job->progressValue) / float(job->progressMax) : 0.0f;
        prKitProgress(kit, over, {.value = done, .indeterminate = job->progressMax == 0});
    }
}

void buildBrowser(PrKit* kit, PrLayoutNode content, AppState& state, Commands& commands) {
    prCommandScope(prKitUi(kit), content, commands.registry, commands.keymap, kContextGallery);
    Gallery& gallery = state.gallery;
    if (gallery.entries.empty()) {
        prKitEmptyState(kit, content, {
            .icon = PrKitIconKind_Folder,
            .title = "The gallery is empty",
            .hint = gallery.dir,
        });
        return;
    }
    PrKitListViewStyle style{.rowHeight = kTileHeight, .tileWidth = kTileWidth, .place = {.flex = PrKitFlex_Fill}};
    PrLayoutNode view = prKitListView(
        kit, content, {.count = uint32_t(gallery.entries.size()), .selection = &gallery.selection}, style);
    PrKitWindow window = prKitListWindow(kit, view);
    for (uint32_t i = window.first; i < window.first + window.count && i < gallery.entries.size(); ++i) {
        const GalleryEntry& entry = gallery.entries[i];
        PrKitTile item{
            .index = i,
            .item = std::hash<std::string>{}(entry.filename),
            .selected = gallery.selected[i] != 0,
        };
        PrLayoutNode tile = prKitListTile(kit, view, item);
        // The thumbnail is an icon until the cache has decoded the image.
        const PrDrawImage* image = imageCacheGet(state.images, entry.path, ImageScale_Thumbnail);
        if (image != nullptr)
            prKitTileImage(kit, tile, {.image = image});
        else
            prKitTileIcon(kit, tile, {.icon = PrKitIconKind_Image}, PrColor{0, 0, 0, 0});
        prKitTileLabel(kit, tile, {.text = entry.filename});
    }
    PrKitSelectionEdit edit{};
    if (prKitSelectionEdited(kit, view, &edit)) {
        std::unique_ptr<bool[]> flags(new bool[gallery.entries.size()]);
        for (size_t i = 0; i < gallery.entries.size(); ++i)
            flags[i] = gallery.selected[i] != 0;
        prKitSelectionApply(&edit, flags.get(), uint32_t(gallery.entries.size()));
        for (size_t i = 0; i < gallery.entries.size(); ++i)
            gallery.selected[i] = flags[i];
        gallery.lead = gallery.selection.lead;
    }
}

void buildMenus(PrKit* kit, PrLayoutNode bar, Commands& commands) {
    struct Menu {
        const char* title;
        std::vector<const char*> items;   // command ids, with null for a separator
    };
    static const Menu kMenus[] = {
        {"Alchemy", {"alchemy.connect", "alchemy.disconnect", nullptr, "view.palette", nullptr, "app.quit"}},
        {
            "Run",
            {
                "alchemy.generate", "alchemy.interrupt", nullptr, "alchemy.mode.generate", "alchemy.mode.upscale",
                "alchemy.mode.outpaint", "alchemy.mode.detail", nullptr, "alchemy.newSeed", "alchemy.lockSeed",
                "alchemy.swapDimensions", "alchemy.clearSource",
            },
        },
        {
            "Image",
            {
                "alchemy.back", "alchemy.forward", "alchemy.previousImage", "alchemy.nextImage", nullptr,
                "alchemy.useAsSource", "alchemy.usePrompt", "alchemy.useSeed", "alchemy.useAspect", "alchemy.useAll",
                nullptr, "alchemy.deleteImage", "alchemy.rescan", "alchemy.openGallery",
            },
        },
        {"View", {"alchemy.toggleInfo", "alchemy.togglePreview"}},
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
                prCommandMenuItem(kit, list, commands.registry, commands.keymap, id);
        }
    }
}

void buildStatus(PrKit* kit, PrLayoutNode parent, AppState& state) {
    PrKitSurfaceStyle bar{
        .box = {
            .direction = PrKitDirection_Row,
            .align = PrKitAlign_Center,
            .gap = kPrKitSpace4,
            .padding = prKitEdgesXY(kPrKitSpace2, kPrKitSpace5),
        },
        .ground = PrKitGround_Header,
    };
    PrLayoutNode status = prKitSurface(kit, parent, {}, bar);
    static const char* const kStates[] = {"Disconnected", "Connecting", "Connected"};
    PrKitTone tones[] = {PrKitTone_Muted, PrKitTone_Warning, PrKitTone_Success};
    prKitLabel(kit, status, {.text = kStates[state.connection]},
        textStyle(PrKitTextRole_Caption, tones[state.connection]));
    PrLayoutNode message = prKitLabel(kit, status, {.text = state.status}, textStyle(PrKitTextRole_BodySmall));
    PrKitPlace fill{.flex = PrKitFlex_Fill};
    prKitPlace(kit, message, &fill);
    if (const Job* job = activeJob(state)) {
        std::string stage = job->node.empty() ? std::string("queued") : job->node;
        if (job->progressMax != 0)
            stage += " " + std::to_string(job->progressValue) + "/" + std::to_string(job->progressMax);
        prKitLabel(kit, status, {.text = stage}, textStyle(PrKitTextRole_Mono, PrKitTone_Accent));
    }
    uint32_t inFlight = runsInFlight(state);
    if (inFlight > 1 || state.queueRemaining > 1) {
        std::string queued = std::to_string(std::max(inFlight, state.queueRemaining)) + " queued";
        prKitLabel(kit, status, {.text = queued}, textStyle(PrKitTextRole_Mono, PrKitTone_Muted));
    }
    std::string count = std::to_string(state.gallery.entries.size()) + " images";
    prKitLabel(kit, status, {.text = count}, textStyle(PrKitTextRole_Mono, PrKitTone_Muted));
}

void buildDeleteDialog(PrKit* kit, PrLayoutNode trigger, AppState& state, View& view) {
    if (state.deleteOpen && !view.deleteShown) {
        prKitSetOpen(kit, trigger, true);
        view.deleteShown = true;
    }
    PrLayoutNode body = prKitDialog(kit, trigger, {.title = "Delete images"}, {});
    if (body == kPrLayoutNodeNone) {
        // A dismissal of the dialog cancels the delete.
        if (view.deleteShown) {
            state.deleteOpen = false;
            view.deleteShown = false;
        }
        return;
    }
    std::string question;
    if (state.deletePaths.size() == 1)
        question = "Delete " + stringOf(pathFilename(state.deletePaths[0])) + "?";
    else
        question = "Delete " + std::to_string(state.deletePaths.size()) + " images?";
    prKitLabel(kit, body, {.text = question}, textStyle(PrKitTextRole_Body));
    prKitLabel(kit, body, {.text = "The files are removed from the gallery's folder, which cannot be undone."},
        PrKitTextStyle{.role = PrKitTextRole_BodySmall, .tone = PrKitTone_Muted, .wrap = PrKitWrap_Wrap});
    PrLayoutNode footer = prKitDialogFooter(kit, body);
    if (prKitActivated(kit, prKitButton(kit, footer, {.text = "Cancel"}, {})))
        prKitSetOpen(kit, trigger, false);
    PrLayoutNode confirm = prKitButton(kit, footer, {.text = "Delete"}, {.variant = PrKitButtonVariant_Danger});
    if (prKitActivated(kit, confirm)) {
        confirmDelete(state);
        prKitSetOpen(kit, trigger, false);
        view.deleteShown = false;
    }
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

void buildView(PrKit* kit, PrLayoutNode root, View& view, Commands& commands) {
    AppState& state = *commands.shell->state;
    prCommandScope(prKitUi(kit), root, commands.registry, commands.keymap, "");
    PrLayoutNode bar = prKitMenuBar(kit, root, {});
    buildMenus(kit, bar, commands);
    if (commands.shell->paletteRequested) {
        prKitSetOpen(kit, bar, true);
        commands.shell->paletteRequested = false;
    }
    PrLayoutNode list = prCommandPalette(kit, bar, commands.registry, commands.keymap, bufferOf(view.paletteQuery));
    if (list == kPrLayoutNodeNone)
        view.paletteQuery.clear();

    PrLayoutNode dock = prKitDockBuild(kit, root, {.dock = view.dock, .describe = describePanel});
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
    buildStatus(kit, root, state);
    buildDeleteDialog(kit, dock, state, view);
}

} // namespace alchemy
