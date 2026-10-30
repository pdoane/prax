// The Studio Kit gallery's view: its sidebar of sections, the scrolling column that holds them, and the helpers that
// the sections share.

#include "examples/studio_gallery/gallery_view.h"

#include "pr/kit_button.h"
#include "pr/kit_choice.h"
#include "pr/layout_query.h"
#include "pr/ui.h"

#include "examples/studio_gallery/gallery_parts.h"
#include "examples/studio_gallery/section_components.h"
#include "examples/studio_gallery/section_controls.h"
#include "examples/studio_gallery/section_data.h"
#include "examples/studio_gallery/section_feedback.h"
#include "examples/studio_gallery/section_foundations.h"

#include <stdio.h>

namespace gallery {

namespace {

// How far past the top of the content a section's top may be and still be the section the sidebar marks, in px.
constexpr float kActiveSlack = 80;

// The width of the sidebar, in px.
constexpr float kSidebarWidth = 240;

// An entry of the sidebar's table of contents, which is also the order of the sections on the page: its section, and
// what builds the section.
struct SidebarEntry {
    const char* PR_NULLABLE group;  // a heading above the entry
    const char* label;
    const char* number;
    Section section;
    void (*build)(PrKit* kit, PrLayoutNode parent, GalleryState& state);
};

// Builds a section that reads no state.
template <void (*Build)(PrKit* kit, PrLayoutNode parent)>
void stateless(PrKit* kit, PrLayoutNode parent, GalleryState&) {
    Build(kit, parent);
}

constexpr SidebarEntry kSidebar[] = {
    {"FOUNDATIONS", "Principles", "00", Section_Principles, stateless<sectionPrinciples>},
    {nullptr, "Color", "01", Section_Color, sectionColor},
    {nullptr, "Type", "02", Section_Type, stateless<sectionType>},
    {nullptr, "Space & Grid", "03", Section_Space, sectionSpace},
    {nullptr, "Icons", "04", Section_Icons, stateless<sectionIcons>},
    {"CONTROLS", "Buttons", "05", Section_Buttons, stateless<sectionButtons>},
    {nullptr, "Inputs", "06", Section_Inputs, sectionInputs},
    {nullptr, "Toggles", "07", Section_Toggles, sectionToggles},
    {nullptr, "Tabs & Segmented", "08", Section_Tabs, sectionTabs},
    {nullptr, "Menus & Popovers", "09", Section_Menus, sectionMenus},
    {"COMPONENTS", "Tree View", "10", Section_Tree, sectionTree},
    {nullptr, "Inspector Rows", "11", Section_Inspector, sectionInspector},
    {nullptr, "Toolbar", "12", Section_Toolbar, sectionToolbar},
    {nullptr, "Tables", "13", Section_Tables, sectionTables},
    {nullptr, "Tags & Badges", "14", Section_Tags, sectionTags},
    {"FEEDBACK", "Progress", "15", Section_Progress, sectionProgress},
    {nullptr, "Tooltips", "16", Section_Tooltips, stateless<sectionTooltips>},
    {nullptr, "Dialogs", "17", Section_Dialogs, sectionDialogs},
    {nullptr, "Status & Console", "18", Section_Status, stateless<sectionStatus>},
    {"DATA", "Collections", "19", Section_Collections, sectionCollections},
    {nullptr, "Timeline", "20", Section_Timeline, sectionTimeline},
    {nullptr, "Images", "21", Section_Images, sectionImages},
    {nullptr, "Node Graph", "22", Section_Graph, sectionGraph},
    {"DIAGNOSTICS", "Duplicate Keys", "23", Section_Keys, sectionKeys},
};

// Finds the section that the content was scrolled to at the last layout: the last whose top is near enough to the
// content's top.
void findActiveSection(PrKit* kit, GalleryState& state) {
    if (state.contentView == kPrLayoutNodeNone)
        return;
    PrLayoutTree* tree = prKitTree(kit);
    PrLayoutRect view = prLayoutNodeBounds(tree, state.contentView, PrLayoutCoordinates_Viewport);
    PrLayoutUnit slack = prLayoutUnitFromPx(kActiveSlack);
    uint32_t active = 0;
    for (uint32_t i = 0; i < Section_Count; ++i) {
        if (state.sectionAnchor[i] == kPrLayoutNodeNone)
            continue;
        PrLayoutRect section = prLayoutNodeBounds(tree, state.sectionAnchor[i], PrLayoutCoordinates_Viewport);
        if (section.bottom.raw > section.top.raw && section.top.raw - view.top.raw <= slack.raw)
            active = i;
    }
    state.activeSection = active;
}

// Creates a row of the sidebar's table of contents, which marks the active section and reveals its section when it is
// activated.
void sidebarRow(PrKit* kit, PrLayoutNode list, const SidebarEntry& entry, GalleryState& state) {
    bool active = state.activeSection == entry.section;
    PrKitBoxStyle row{
        .direction = PrKitDirection_Row,
        .align = PrKitAlign_Center,
        .gap = kPrKitSpace3,
        .padding = prKitEdgesXY(0, kPrKitSpace4),
        .place = {.height = prKitPx(kPrKitHeightSm), .flex = PrKitFlex_Fixed},
    };
    PrLayoutNode node = prKitSelectable(kit, list, {.selected = active}, row);
    PrKitTextStyle label{.tone = PrKitTone(active ? PrKitTone_Primary : PrKitTone_Secondary), .wrap = PrKitWrap_Clip};
    fillLabel(kit, node, entry.label, label);
    prKitLabel(kit, node, {.text = entry.number}, active ? kMetaAccent : kMeta);
    if (prKitActivated(kit, node) && state.sectionAnchor[entry.section] != kPrLayoutNodeNone)
        prUiReveal(prKitUi(kit), state.sectionAnchor[entry.section]);
}

void buildSidebar(PrKit* kit, PrLayoutNode parent, GalleryState& state) {
    findActiveSection(kit, state);
    PrKitSurfaceStyle sidebar{.ground = PrKitGround_App, .rules = PrKitRuleFlag_Right};
    sidebar.box.overflow = PrKitOverflow_Clip;
    sidebar.box.place.width = prKitPx(kSidebarWidth);
    sidebar.box.place.flex = PrKitFlex_Fixed;
    PrLayoutNode side = prKitSurface(kit, parent, {}, sidebar);

    PrKitBoxStyle head{.gap = kPrKitSpace3, .padding = {kPrKitSpace5, kPrKitSpace4, kPrKitSpace5, kPrKitSpace4}};
    head.place.flex = PrKitFlex_Fixed;
    PrLayoutNode heading = prKitBox(kit, side, {}, head);
    prKitLabel(kit, heading, {.text = "PRAX DESIGN SYSTEM"}, kCaptionFaint);
    const PrKitChoice themes[] = {{.label = "System"}, {.label = "Dark"}, {.label = "Light"}};
    segmented(kit, heading, themes, 3, &state.theme, {.size = PrKitControlSize_Sm});

    PrKitBoxStyle scroll{.overflow = PrKitOverflow_ScrollY};
    scroll.place.flex = PrKitFlex_Fill;
    PrLayoutNode list = prKitBox(kit, side, {}, scroll);
    for (const SidebarEntry& entry : kSidebar) {
        if (entry.group != nullptr) {
            PrKitBoxStyle group{.padding = {kPrKitSpace6, kPrKitSpace4, kPrKitSpace2, kPrKitSpace4}};
            group.place.flex = PrKitFlex_Fixed;
            prKitLabel(kit, prKitBox(kit, list, {}, group), {.text = entry.group}, kCaptionFaint);
        }
        sidebarRow(kit, list, entry, state);
    }
}

// Creates the section of `id` under the content, and records its node for the sidebar.
PrLayoutNode sectionAnchor(PrKit* kit, PrLayoutNode content, GalleryState& state, Section id) {
    PrLayoutNode section = prKitBox(kit, content, {}, {.gap = kPrKitSpace3});
    state.sectionAnchor[id] = section;
    // The reveal waits for the section's first layout, whose geometry it scrolls by.
    if (state.reveal == id) {
        PrLayoutRect laid = prLayoutNodeBounds(prKitTree(kit), section, PrLayoutCoordinates_Viewport);
        if (laid.bottom.raw > laid.top.raw) {
            prUiReveal(prKitUi(kit), section);
            state.reveal = Section_Count;
        } else {
            prUiRequestFrame(prKitUi(kit));
        }
    }
    return section;
}

} // namespace

void buildGallery(PrKit* kit, PrLayoutNode ground, GalleryState& state) {
    PrKitBoxStyle page{.direction = PrKitDirection_Row};
    page.place.flex = PrKitFlex_Fill;
    PrLayoutNode row = prKitBox(kit, ground, {}, page);
    buildSidebar(kit, row, state);
    PrKitBoxStyle scroll{
        .overflow = PrKitOverflow_ScrollY,
        .gap = kPrKitSpace16,
        .padding = {kPrKitSpace10 * 2, kPrKitSpace12, kPrKitSpace16 * 3, kPrKitSpace12},
    };
    scroll.place.flex = PrKitFlex_Fill;
    PrLayoutNode content = prKitBox(kit, row, {}, scroll);
    state.contentView = content;
    for (const SidebarEntry& entry : kSidebar)
        entry.build(kit, sectionAnchor(kit, content, state, entry.section), state);
}

} // namespace gallery
