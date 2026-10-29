// The gallery's feedback: progress, tooltips, dialogs, the status bar and console, images, and duplicate keys.

#include "examples/studio_gallery/section_feedback.h"

#include "pr/kit_button.h"
#include "pr/kit_choice.h"
#include "pr/kit_data.h"
#include "pr/kit_icon.h"
#include "pr/kit_image.h"
#include "pr/kit_menu.h"
#include "pr/kit_surface.h"
#include "pr/ui.h"

#include "examples/studio_gallery/gallery_parts.h"

namespace gallery {

namespace {

constexpr PrKitTextStyle kBodySmall = {.role = PrKitTextRole_BodySmall, .tone = PrKitTone_Muted};

// The side of the square frames that the pictures are fitted into, in px.
constexpr float kFrameSize = 148;

// Creates a square frame for a picture on the viewport's ground, which shows where a fit leaves room.
PrLayoutNode framed(PrKit* kit, PrLayoutNode parent) {
    PrKitSurfaceStyle surface{.ground = PrKitGround_Viewport};
    surface.box.place = {.width = prKitPx(kFrameSize), .height = prKitPx(kFrameSize), .flex = PrKitFlex_Fixed,
                         .align = PrKitAlign_Start};
    return prKitSurface(kit, parent, {}, surface);
}

// A button that shows a tooltip.
struct TipButton {
    const char* label;
    const char* shortcut;
    PrKitIconKind icon;
};

constexpr TipButton kTipButtons[] = {
    {"Move Tool", "W", PrKitIconKind_Move},           {"Rotate Tool", "E", PrKitIconKind_Rotate},
    {"Scale Tool", "R", PrKitIconKind_Scale},         {"Toggle Visibility", "", PrKitIconKind_Eye},
    {"Lock Selection", "", PrKitIconKind_Lock},
};

// The console's seeded lines.
constexpr PrKitLogLine kLog[] = {
    {0, "12:04:18.2", "Scene Level_01 loaded", "SceneManager", PrKitLogLevel_Info},
    {0, "12:04:18.3", "Compiling 142 shaders", "ShaderCache", PrKitLogLevel_Info},
    {0, "12:04:19.4", "Compilation complete in 1.1 s", "ShaderCache", PrKitLogLevel_Info},
    {0, "12:04:21.0", "Texture env_diffuse.ktx2 has non-power-of-two dimensions", "AssetImport",
     PrKitLogLevel_Warning},
    {0, "12:04:25.6", "Entity Enemy_04 is missing a Rigidbody", "PhysicsSystem",
     PrKitLogLevel_Error},
    {0, "12:04:25.7", "LOD group reports no levels for tree_pine_03.mesh", "Renderer",
     PrKitLogLevel_Warning},
    {0, "12:04:26.0", "Autosave complete", "Autosave", PrKitLogLevel_Info},
};

// Creates a row of `gap`, centered across.
PrLayoutNode specimenRow(PrKit* kit, PrLayoutNode parent, float gap) {
    PrKitBoxStyle row{.direction = PrKitDirection_Row, .align = PrKitAlign_Center, .wrap = true, .gap = gap};
    return prKitBox(kit, parent, {}, row);
}

// Places `node` to take the room that its parent leaves.
void fill(PrKit* kit, PrLayoutNode node) {
    PrKitPlace place{.flex = PrKitFlex_Fill};
    prKitPlace(kit, node, &place);
}

} // namespace

void sectionProgress(PrKit* kit, PrLayoutNode parent, GalleryState& state) {
    ProgressState& progress = state.progress;
    PrLayoutNode section = sectionFrame(
        kit, parent, "15", "Progress", "Work in flight, and a reading against a budget.",
        "A determinate bar states a fraction, an indeterminate one states only that work is running, and a meter "
              "lights the segments its value covers. Each of the moving ones asks for the frames that move it.");
    {
        PrLayoutNode panel = demoPanel(kit, section, "PROGRESS", "DETERMINATE . INDETERMINATE", false);
        PrLayoutNode bake = captionRow(kit, panel, "BAKE", 92);
        fill(kit, prKitProgress(kit, bake, {.value = progress.bake}));
        char percent[16];
        prKitLabel(kit, bake, {.text = format(percent, sizeof percent, "%.0f%%", double(progress.bake * 100))},
                   kMonoMuted);
        PrLayoutNode scan = captionRow(kit, panel, "SCANNING", 92);
        fill(kit, prKitProgress(kit, scan, {.indeterminate = true}));
        prKitLabel(kit, scan, {.text = "no fraction to state"}, kBodySmall);
        PrLayoutNode steps = specimenRow(kit, panel, kPrKitSpace3);
        const float fractions[] = {0, 0.25f, 0.5f, 0.75f, 1};
        for (float fraction : fractions) {
            PrLayoutNode node = prKitProgress(kit, steps, {.value = fraction});
            PrKitPlace width{.width = prKitPx(80), .flex = PrKitFlex_Fixed};
            prKitPlace(kit, node, &width);
        }
    }
    {
        PrLayoutNode panel = demoPanel(kit, section, "METERS", "SEGMENTS . TONE", false);
        fill(kit, prKitMeter(kit, captionRow(kit, panel, "MEMORY", 92), {.value = progress.memory},
                             {.tone = PrKitTone_Accent}));
        fill(kit, prKitMeter(kit, captionRow(kit, panel, "FRAME MS", 92),
                             {.value = progress.budget, .low = 0, .high = 16.6f}, {.tone = PrKitTone_Warning}));
        fill(kit, prKitMeter(kit, captionRow(kit, panel, "ERRORS", 92), {.value = 1, .low = 0, .high = 8},
                             {.tone = PrKitTone_Danger}));
    }
    {
        PrLayoutNode panel = demoPanel(kit, section, "SPINNER", "SIZES", false);
        PrLayoutNode row = specimenRow(kit, panel, kPrKitSpace6);
        const float sizes[] = {12, 16, 24, 32};
        for (float size : sizes)
            prKitSpinner(kit, row, {}, {.size = size});
        prKitLabel(kit, row, {.text = "an arc that turns while the work runs"}, kBodySecondary);
    }
    // The bake follows the round's time, and moves in the frames that the moving widgets beside it ask for.
    if (progress.running) {
        double turns = prUiCurrentRound(prKitUi(kit))->time * 0.25;
        progress.bake = float(turns - double(int64_t(turns)));
    }
}

void sectionTooltips(PrKit* kit, PrLayoutNode parent) {
    PrLayoutNode section = sectionFrame(
        kit, parent, "16", "Tooltips", "A name and its chord, after a short rest.",
        "A tooltip shows once the pointer has rested on its anchor, places itself from that anchor's last box "
              "and stays inside the frame. The anatomy beside it is the same chip, drawn in the flow.");
    {
        PrLayoutNode panel = demoPanel(kit, section, "ANATOMY", "IN THE FLOW", false);
        PrLayoutNode row = specimenRow(kit, panel, kPrKitSpace3);
        for (const TipButton& tip : kTipButtons) {
            PrKitSurfaceStyle chip{.ground = PrKitGround_Hud, .rules = PrKitRuleFlag_All};
            chip.box.direction = PrKitDirection_Row;
            chip.box.align = PrKitAlign_Center;
            chip.box.gap = kPrKitSpace3;
            chip.box.padding = prKitEdgesXY(kPrKitSpace1, kPrKitSpace3);
            PrLayoutNode node = prKitSurface(kit, row, {}, chip);
            prKitLabel(kit, node, {.text = tip.label}, kBodySmall);
            if (tip.shortcut[0] != 0)
                prKitKbd(kit, node, {.text = tip.shortcut});
        }
    }
    PrLayoutNode panel = demoPanel(kit, section, "HOVER", "DELAY . SHORTCUT", false);
    PrLayoutNode row = specimenRow(kit, panel, kPrKitSpace3);
    for (const TipButton& tip : kTipButtons) {
        PrKitButton content{.icon = tip.icon};
        PrLayoutNode node = prKitButton(kit, row, content, {});
        PrKitTooltip tooltip{.text = tip.label, .shortcut = tip.shortcut};
        prKitTooltip(kit, node, tooltip);
    }
    prKitLabel(kit, panel, {.text = "REST ON A BUTTON ABOVE"}, kCaption);
}

void sectionDialogs(PrKit* kit, PrLayoutNode parent, GalleryState& state) {
    PrLayoutNode section = sectionFrame(
        kit, parent, "17", "Dialogs", "A blocking surface for what cannot be undone.",
        "A dialog is a modal carrying a titled card: the scrim dims the page and takes the presses that miss "
              "it, the close control and Escape take it down, and the footer holds the buttons that answer it.");
    {
        PrLayoutNode panel = demoPanel(kit, section, "CONFIRM", "CENTER", false);
        PrLayoutNode row = specimenRow(kit, panel, kPrKitSpace3);
        PrLayoutNode trigger = prKitButton(kit, row, {.text = "Delete 3 Entities..."}, kDanger);
        if (prKitActivated(kit, trigger))
            prKitSetOpen(kit, trigger, true);
        prKitLabel(kit, row, {.text = "the card carries a header, a body and a footer"}, kBodySecondary);
        PrKitDialog dialog{.title = "Delete the selected entities?"};
        PrLayoutNode body = prKitDialog(kit, trigger, dialog, {});
        if (body != kPrLayoutNodeNone) {
            prKitLabel(kit, body, {.text = "Three entities and their children go with this. References from other scenes are left "
                             "broken."}, kBodySecondary);
            PrKitSurfaceStyle callout{.ground = PrKitGround_Header};
            callout.box.gap = kPrKitSpace1;
            callout.box.padding = prKitEdgesXY(kPrKitSpace2, kPrKitSpace4);
            PrLayoutNode lines = prKitSurface(kit, body, {}, callout);
            const char* const names[] = {"Player . 0x7FF8_A2B0", "Enemy_04 . 0x7FF8_B001",
                                         "LightProbe_Grid . 0x7FF8_B112"};
            for (const char* name : names)
                prKitLabel(kit, lines, {.text = name}, kMonoSecondary);
            prKitCheckbox(kit, body, {.value = &state.dialog.deleteOrphans, .label = "Also delete orphaned assets"},
                          {});
            PrLayoutNode footer = prKitDialogFooter(kit, body);
            bool cancelled = prKitActivated(kit, prKitButton(kit, footer, {.text = "Cancel"}, {}));
            bool confirmed =
                prKitActivated(kit, prKitButton(kit, footer, {.text = "Delete 3 Entities"}, kDanger));
            if (cancelled || confirmed)
                prKitSetOpen(kit, trigger, false);
        }
    }
    {
        PrLayoutNode panel = demoPanel(kit, section, "TOP SHEET", "TOP", false);
        PrLayoutNode row = specimenRow(kit, panel, kPrKitSpace3);
        PrLayoutNode trigger = prKitButton(kit, row, {.text = "Open Top Sheet..."}, {});
        if (prKitActivated(kit, trigger))
            prKitSetOpen(kit, trigger, true);
        prKitLabel(kit, row, {.text = "a topped dialog drops from the top edge, as a launcher does"},
                   kBodySecondary);
        PrKitDialog dialog{.title = "Go to File"};
        PrKitDialogStyle top{.modal = {.placement = PrKitModalPlacement_Top}};
        PrLayoutNode body = prKitDialog(kit, trigger, dialog, top);
        if (body != kPrLayoutNodeNone) {
            prKitLabel(kit, body, {.text = "A top-placed card reads as a launcher rather than a question, which is the shape the "
                             "command palette takes."}, kBodySecondary);
            PrLayoutNode footer = prKitDialogFooter(kit, body);
            if (prKitActivated(kit, prKitButton(kit, footer, {.text = "Close"}, {})))
                prKitSetOpen(kit, trigger, false);
        }
    }
    {
        // A modal places and dims, and its content is the caller's own.
        PrLayoutNode panel = demoPanel(kit, section, "PLAIN MODAL", "SCRIM . CONTENT", false);
        PrLayoutNode row = specimenRow(kit, panel, kPrKitSpace3);
        PrLayoutNode trigger = prKitButton(kit, row, {.text = "Open Modal..."}, {});
        if (prKitActivated(kit, trigger))
            prKitSetOpen(kit, trigger, true);
        prKitLabel(kit, row, {.text = "no header and no footer: the content is the caller's own"},
                   kBodySecondary);
        PrLayoutNode content = prKitModal(kit, trigger, {}, {});
        if (content != kPrLayoutNodeNone) {
            PrKitBoxStyle box{.gap = kPrKitSpace4, .padding = prKitEdgesAll(kPrKitSpace8)};
            box.place.width = prKitPx(360);
            PrLayoutNode frame = card(kit, content, 0, box);
            PrLayoutNode heading = prKitBox(kit, frame, {}, {.direction = PrKitDirection_Row, .gap = kPrKitSpace3});
            prKitIcon(kit, heading, {.icon = PrKitIconKind_Info}, {.size = 16, .tone = PrKitTone_Accent});
            prKitLabel(kit, heading, {.text = "A frame of the caller's own"}, kStrong);
            prKitLabel(kit, frame, {.text = "The modal places and dims; everything inside it is declared here."},
                       kBodySecondary);
            PrKitBoxStyle actions{.direction = PrKitDirection_Row, .justify = PrKitJustify_End, .gap = kPrKitSpace3};
            if (prKitActivated(kit, prKitButton(kit, prKitBox(kit, frame, {}, actions), {.text = "Close"}, {})))
                prKitSetOpen(kit, trigger, false);
        }
    }
}

void sectionStatus(PrKit* kit, PrLayoutNode parent) {
    PrLayoutNode section = sectionFrame(kit, parent, "18", "Status Bar & Console",
                                        "The two places the engine talks back.", {});
    PrLayoutNode panel = demoPanel(kit, section, "STATUS BAR", "BOTTOM CHROME", true);
    PrKitSurfaceStyle strip{.ground = PrKitGround_Header, .rules = PrKitRuleFlag_Top};
    strip.box.direction = PrKitDirection_Row;
    strip.box.align = PrKitAlign_Center;
    strip.box.gap = kPrKitSpace4;
    strip.box.padding = prKitEdgesXY(0, kPrKitSpace4);
    strip.box.overflow = PrKitOverflow_Clip;
    strip.box.place.height = prKitPx(kPrKitHeightSm);
    strip.box.place.flex = PrKitFlex_Fixed;
    PrLayoutNode bar = prKitSurface(kit, panel, {}, strip);
    auto status = [&](PrLayoutNode at, PrStringView text, PrKitTone tone) {
        PrKitTextStyle style{.role = PrKitTextRole_Caption, .tone = tone};
        prKitLabel(kit, at, {.text = text}, style);
    };
    PrLayoutNode build = prKitBox(kit, bar, {}, {.direction = PrKitDirection_Row, .gap = kPrKitSpace2});
    prKitIcon(kit, build, {.icon = PrKitIconKind_Check}, {.size = 10, .tone = PrKitTone_Success});
    status(build, "BUILD OK", PrKitTone_Success);
    const char* const items[] = {"Level_01.pr", "ENTITIES 248 . DRAW 1,412 . TRIS 2.4M", "60.00 fps"};
    for (const char* item : items) {
        prKitDivider(kit, bar, {});
        status(bar, item, PrKitTone_Muted);
    }
    prKitSpacer(kit, bar, {});
    status(bar, "LINE 42 . COL 18", PrKitTone_Muted);
    prKitDivider(kit, bar, {});
    PrLayoutNode warn = prKitBox(kit, bar, {}, {.direction = PrKitDirection_Row, .gap = kPrKitSpace2});
    prKitIcon(kit, warn, {.icon = PrKitIconKind_Alert}, {.size = 10, .tone = PrKitTone_Warning});
    status(warn, "2", PrKitTone_Warning);
    PrLayoutNode error = prKitBox(kit, bar, {}, {.direction = PrKitDirection_Row, .gap = kPrKitSpace2});
    prKitIcon(kit, error, {.icon = PrKitIconKind_Alert}, {.size = 10, .tone = PrKitTone_Danger});
    status(error, "1", PrKitTone_Danger);
    prKitDivider(kit, bar, {});
    status(bar, "UTF-8", PrKitTone_Muted);

    PrLayoutNode tiles = demoPanel(kit, section, "STAT TILES", "READOUTS", false);
    PrKitBoxStyle line{.direction = PrKitDirection_Row, .align = PrKitAlign_Stretch, .gap = kPrKitSpace3};
    PrLayoutNode readouts = prKitBox(kit, tiles, {}, line);
    const PrKitStatTile stats[] = {
        {.label = "FRAME", .value = "16.6 ms", .note = "60 fps target"},
        {.label = "DRAW CALLS", .value = "1,412", .note = "248 entities"},
        {.label = "GPU", .value = "11.2 ms", .note = "of the 16.6 budget",
         .noteTone = PrKitTone_Warning},
        {.label = "ERRORS", .value = "1", .note = "since the last build",
         .noteTone = PrKitTone_Danger},
    };
    for (const PrKitStatTile& stat : stats)
        prKitStatTile(kit, readouts, stat);

    PrLayoutNode console = demoPanel(kit, section, "CONSOLE", "LOG LEVELS", true);
    PrKitSurfaceStyle log{.ground = PrKitGround_Panel};
    log.box.overflow = PrKitOverflow_ScrollY;
    log.box.place.height = prKitPx(180);
    log.box.place.flex = PrKitFlex_Fixed;
    PrLayoutNode lines = prKitSurface(kit, console, {}, log);
    for (const PrKitLogLine& entry : kLog)
        prKitLogLine(kit, lines, entry);
    PrLayoutNode compact = demoPanel(kit, section, "COMPACT CONSOLE", "MESSAGE ONLY", true);
    const PrKitLogLine messages[] = {
        {.message = "Connected to the build server"},
        {.message = "Shader cache 96 percent warm"},
        {.message = "Package manifest older than the lockfile", .level = PrKitLogLevel_Warning},
        {.message = "Upload failed: connection reset", .level = PrKitLogLevel_Error},
    };
    for (const PrKitLogLine& entry : messages)
        prKitLogLine(kit, compact, entry);
}

void sectionImages(PrKit* kit, PrLayoutNode parent, GalleryState& state) {
    PrLayoutNode section = sectionFrame(
        kit, parent, "21", "Images", "One wide picture in four bounded frames.",
        "A fit places the picture inside the box that layout already sized; it never sizes the box. The "
              "alignment slides the picture within the box, which moves the part of a cropped picture that shows.");
    const PrDrawImage* picture = galleryPicture(state);
    struct Specimen {
        const char* title;
        const char* tags;
        PrKitImageFit fit;
        const char* caption;
    };
    const Specimen specimens[] = {
        {"FILL", "DEFAULT", PrKitImageFit_Fill, "Stretched over the frame; the ratio is not kept."},
        {"CONTAIN", "FIT", PrKitImageFit_Contain, "The whole picture inside the frame, letterboxed."},
        {"COVER", "CROP", PrKitImageFit_Cover, "The frame filled edge to edge; the overflow is cropped."},
        {"SCALE DOWN", "FIT", PrKitImageFit_ScaleDown, "Contain, but never larger than the picture itself."},
    };
    PrKitPlace frame{.width = prKitPercent(100), .height = prKitPercent(100), .flex = PrKitFlex_Fixed};
    Grid grid = gridBegin(kit, section, 4, kPrKitSpace6, false);
    for (const Specimen& specimen : specimens) {
        PrLayoutNode panel = demoPanel(kit, gridCell(grid), specimen.title, specimen.tags, false);
        prKitImage(kit, framed(kit, panel), {.image = picture}, {.fit = specimen.fit, .place = frame});
        prKitLabel(kit, panel, {.text = specimen.caption}, kBodySmall);
    }
    gridEnd(grid);

    PrLayoutNode panel = demoPanel(kit, section, "ALIGNMENT", "COVER", false);
    PrKitBoxStyle line{.direction = PrKitDirection_Row, .align = PrKitAlign_Start, .gap = kPrKitSpace6};
    PrLayoutNode row = prKitBox(kit, panel, {}, line);
    const PrKitAlign aligns[] = {PrKitAlign_Start, PrKitAlign_Center, PrKitAlign_End};
    const char* const names[] = {"START", "CENTER", "END"};
    for (uint32_t i = 0; i < 3; ++i) {
        PrKitBoxStyle cell{.align = PrKitAlign_Center, .gap = kPrKitSpace2};
        PrLayoutNode column = prKitBox(kit, row, {}, cell);
        PrKitImageStyle cover{.fit = PrKitImageFit_Cover, .alignX = aligns[i], .place = frame};
        prKitImage(kit, framed(kit, column), {.image = picture}, cover);
        prKitLabel(kit, column, {.text = names[i]}, kCaption);
    }
}

void sectionKeys(PrKit* kit, PrLayoutNode parent, GalleryState& state) {
    PrLayoutNode section = sectionFrame(
        kit, parent, "23", "Duplicate Keys", "A key names one node of a round.",
        "Two widgets of one round that share a key are a caller's error. Layout makes the second node without the "
              "key, so the tree stays whole, and lib/ui outlines both nodes in the next round.");
    PrLayoutNode panel = demoPanel(kit, section, "ROWS", "LIVE", false);
    prKitToggle(kit, panel, {.value = &state.keys.duplicate, .label = "Give the third row the first row's key"},
                {});
    PrLayoutNode rows = prKitBox(kit, panel, {}, {.gap = kPrKitSpace2});
    const char* const names[] = {"row-a", "row-b", "row-c"};
    const char* const labels[] = {"Transform", "Mesh Renderer", "Rigidbody"};
    for (uint32_t i = 0; i < 3; ++i) {
        const char* name = i == 2 && state.keys.duplicate ? names[0] : names[i];
        uint64_t key = prUiKey(prKitUi(kit), rows, name);
        PrKitSurfaceStyle row{.ground = PrKitGround_Raised};
        row.box.padding = prKitEdgesXY(kPrKitSpace3, kPrKitSpace4);
        PrLayoutNode card = prKitSurface(kit, rows, {.key = key}, row);
        prKitLabel(kit, card, {.text = labels[i]}, {});
    }
}

} // namespace gallery
