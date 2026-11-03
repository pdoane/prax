// The Studio Kit gallery's model: its seed data, and the helpers that edit it.

#include "examples/studio_gallery/gallery_state.h"

#include "pr/count_of.h"
#include "pr/float.h"

#include <ctype.h>
#include <stdio.h>

namespace gallery {

using namespace pr;

namespace {

// A row of the seeded scene tree.
struct TreeSeed {
    const char* name;
    const char* meta;
    const char* badge;
    uint32_t depth;
    PrKitIconKind icon;
    PrKitTone badgeTone;
    bool open;
    bool hidden;
};

constexpr TreeSeed kTreeSeeds[] = {
    {"World", "", "", 0, PrKitIconKind_FolderOpen, PrKitTone_Muted, true, false},
    {"Environment", "", "", 1, PrKitIconKind_Folder, PrKitTone_Muted, true, false},
    {"Directional Light", "Light", "", 2, PrKitIconKind_Light, PrKitTone_Muted, false, false},
    {"Sky Dome", "Mesh", "", 2, PrKitIconKind_Sphere, PrKitTone_Muted, false, false},
    {"Fog Volume", "Volume", "W", 2, PrKitIconKind_Cube, PrKitTone_Warning, false, true},
    {"Player", "", "", 1, PrKitIconKind_Cube, PrKitTone_Muted, true, false},
    {"MainCamera", "Camera", "", 2, PrKitIconKind_Camera, PrKitTone_Muted, false, false},
    {"PlayerMesh", "Mesh", "", 2, PrKitIconKind_Cube, PrKitTone_Muted, false, false},
    {"Controller", "Script", "", 2, PrKitIconKind_Script, PrKitTone_Muted, false, false},
    {"Level_01", "", "", 1, PrKitIconKind_Folder, PrKitTone_Muted, false, false},
    {"Changes", "", "", 0, PrKitIconKind_FolderOpen, PrKitTone_Muted, true, false},
    {"world.scene", "", "M", 1, PrKitIconKind_None, PrKitTone_Warning, false, false},
    {"player.script", "", "A", 1, PrKitIconKind_None, PrKitTone_Success, false, false},
    {"old_sky.mat", "", "D", 1, PrKitIconKind_None, PrKitTone_Danger, false, false},
    {"level_01.scene", "was level_1.scene", "R", 1, PrKitIconKind_None, PrKitTone_Accent, false, false},
    {
        "a_very_long_component_name_that_overflows_the_row.scene", "+128 -64", "M", 1, PrKitIconKind_None,
        PrKitTone_Warning, false, false,
    },
};

// Returns one past the last node of the subtree at `start`.
size_t subtreeEnd(const std::vector<TreeNode>& nodes, size_t start) {
    size_t end = start + 1;
    while (end < nodes.size() && nodes[end].depth > nodes[start].depth)
        ++end;
    return end;
}

size_t indexOfKey(const std::vector<TreeNode>& nodes, uint32_t key) {
    for (size_t i = 0; i < nodes.size(); ++i) {
        if (nodes[i].key == key)
            return i;
    }
    return nodes.size();
}

// Finds which nodes have children, and closes the nodes that have none.
void markChildren(std::vector<TreeNode>& nodes) {
    for (size_t i = 0; i < nodes.size(); ++i) {
        nodes[i].hasChildren = i + 1 < nodes.size() && nodes[i + 1].depth > nodes[i].depth;
        if (!nodes[i].hasChildren)
            nodes[i].open = false;
    }
}

int compareText(const char* a, const char* b) {
    for (uint32_t i = 0;; ++i) {
        unsigned char left = static_cast<unsigned char>(a[i]);
        unsigned char right = static_cast<unsigned char>(b[i]);
        if (left != right)
            return left < right ? -1 : 1;
        if (left == 0)
            return 0;
    }
}

int compareKeys(uint64_t a, uint64_t b) {
    return a < b ? -1 : a > b ? 1 : 0;
}

bool assetLess(const AssetRow& a, const AssetRow& b, AssetSort column, bool ascending) {
    int order = 0;
    switch (column) {
        case AssetSort_Type:
            order = compareText(a.type, b.type);
            break;
        case AssetSort_Size:
            order = compareKeys(a.sizeBytes, b.sizeBytes);
            break;
        case AssetSort_Modified:
            order = compareKeys(a.modifiedKey, b.modifiedKey);
            break;
        default:
            order = compareText(a.name, b.name);
            break;
    }
    return ascending ? order < 0 : order > 0;
}

// The kinds of the generated assets.
struct AssetKind {
    const char* type;
    const char* extension;
    PrKitIconKind icon;
};

constexpr AssetKind kAssetKinds[] = {
    {"Mesh", "mesh", PrKitIconKind_Cube},      {"Texture", "ktx2", PrKitIconKind_Image},
    {"Material", "material", PrKitIconKind_Sphere}, {"Script", "lua", PrKitIconKind_Script},
    {"Audio", "ogg", PrKitIconKind_Audio},     {"Camera", "camera", PrKitIconKind_Camera},
    {"Light", "light", PrKitIconKind_Light},
};

// The words of the generated assets' names.
constexpr const char* kAssetQualifiers[] = {
    "rusty",  "oak",      "granite", "coastal",   "ruined", "gilded",
    "frozen", "scorched", "hollow",  "salvaged",  "painted", "weathered",
    "brass",  "crimson",  "quiet",   "distant",
};
constexpr const char* kAssetNouns[] = {
    "crate",  "bulkhead", "banner", "lantern",  "girder", "console",
    "statue", "barrel",   "hatch",  "railing",  "terminal", "pillar",
    "canopy", "beacon",   "bridge", "workbench",
};

// A generator whose sequence is the same on every run, so that the collection is too.
struct AssetRandom {
    uint64_t state;

    uint64_t next() {
        state += 0x9e3779b97f4a7c15ull;
        uint64_t value = state;
        value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ull;
        value = (value ^ (value >> 27)) * 0x94d049bb133111ebull;
        return value ^ (value >> 31);
    }

    uint32_t below(uint32_t bound) {
        return uint32_t(next() % bound);
    }
};

// Returns a byte count in the largest unit that keeps a whole part.
std::string sizeLabel(uint64_t bytes) {
    char text[16];
    if (bytes >= 1024 * 1024)
        snprintf(text, sizeof text, "%.1f MB", double(bytes) / (1024.0 * 1024.0));
    else if (bytes >= 1024)
        snprintf(text, sizeof text, "%.0f KB", double(bytes) / 1024.0);
    else
        snprintf(text, sizeof text, "%llu B", static_cast<unsigned long long>(bytes));
    return text;
}

void seedCollections(CollectionsState& collections) {
    AssetRandom random{0x5eed1e55c0ffee11ull};
    collections.assets.clear();
    collections.assets.reserve(kCollectionGroups * kCollectionGroupSize);
    for (uint32_t i = 0; i < kCollectionGroups * kCollectionGroupSize; ++i) {
        const AssetKind& kind = kAssetKinds[random.below(countOf(kAssetKinds))];
        const char* qualifier = kAssetQualifiers[random.below(countOf(kAssetQualifiers))];
        const char* noun = kAssetNouns[random.below(countOf(kAssetNouns))];
        char name[64];
        snprintf(name, sizeof name, "%s_%s_%02u.%s", qualifier, noun, random.below(100), kind.extension);
        CollectionAsset asset;
        asset.name = name;
        asset.type = kind.type;
        asset.icon = kind.icon;
        // The magnitude is chosen first, so that the sizes spread across their units.
        uint32_t magnitude = 9 + random.below(16);
        asset.sizeBytes = 64ull + random.next() % (1ull << magnitude);
        asset.sizeText = sizeLabel(asset.sizeBytes);
        uint64_t tint = random.next();
        asset.tint = PrColor{
            0.27f + float(tint & 0x6f) / 512.0f, 0.27f + float((tint >> 8) & 0x6f) / 512.0f,
            0.27f + float((tint >> 16) & 0x6f) / 512.0f, 1.0f,
        };
        collections.assets.push_back(asset);
    }
}

bool containsIgnoreCase(const char* text, const std::string& needle) {
    if (needle.empty())
        return true;
    std::string owned(text);
    for (size_t i = 0; i + needle.size() <= owned.size(); ++i) {
        size_t j = 0;
        while (j < needle.size() && tolower(owned[i + j]) == tolower(needle[j]))
            ++j;
        if (j == needle.size())
            return true;
    }
    return false;
}

// A block of the timeline's clip.
struct BlockSeed {
    const char* label;
    uint32_t track;
    float start;
    float duration;
    PrColor color;
};

constexpr BlockSeed kBlockSeeds[] = {
    {"Idle", 0, 0.0f, 3.0f, {.38f, .58f, .82f, 1.0f}},
    {"Walk", 0, 3.25f, 2.5f, {.38f, .58f, .82f, 1.0f}},
    {"Twist", 1, 1.0f, 2.0f, {.59f, .46f, .78f, 1.0f}},
    {"Wave", 2, 2.5f, 3.0f, {.82f, .55f, .35f, 1.0f}},
};

constexpr const char* kTrackSeeds[] = {"Root", "Spine", "Arm.L", "Events"};

// The times of the events on the last track, in seconds.
constexpr float kMarkerSeeds[] = {1.5f, 4.0f, 5.5f};

void seedTimeline(TimelineState& timeline) {
    for (const char* name : kTrackSeeds)
        timeline.tracks.push_back(PrKitTimelineTrack{.name = name});
    for (const BlockSeed& seed : kBlockSeeds) {
        timeline.blocks.push_back(PrKitTimelineBlock{
            .id = timeline.nextId++,
            .track = seed.track,
            .start = seed.start,
            .duration = seed.duration,
            .color = seed.color,
            .label = seed.label,
        });
    }
    for (float time : kMarkerSeeds)
        timeline.markers.push_back(PrKitTimelineMarker{.track = 3, .time = time, .color = {.86f, .38f, .38f, 1}});
}

// The ports of the graph's nodes.
constexpr PrKitGraphPort kSamplerOut[] = {
    {.id = 1, .type = PortType_Color, .label = "rgb"},
    {.id = 2, .type = PortType_Scalar, .label = "alpha"},
};
constexpr PrKitGraphPort kNoiseOut[] = {{.id = 1, .type = PortType_Scalar, .label = "value"}};
constexpr PrKitGraphPort kBlendIn[] = {
    {.id = 1, .type = PortType_Color, .label = "a"},
    {.id = 2, .type = PortType_Color, .label = "b"},
    {.id = 3, .type = PortType_Scalar, .label = "mix"},
};
constexpr PrKitGraphPort kBlendOut[] = {{.id = 1, .type = PortType_Color, .label = "out"}};
constexpr PrKitGraphPort kSurfaceIn[] = {
    {.id = 1, .type = PortType_Color, .label = "color"},
    {.id = 2, .type = PortType_Scalar, .label = "opacity"},
    {.id = 3, .type = PortType_Vector, .label = "normal"},
};

void seedGraph(GraphState& graph) {
    graph.nodes = {
        {
            .id = 1,
            .position = {24, 36},
            .title = "Sampler",
            .meta = "texture2d",
            .outputs = kSamplerOut,
            .outputCount = 2,
        },
        {.id = 2, .position = {40, 190}, .title = "Noise", .meta = "generator", .outputs = kNoiseOut, .outputCount = 1},
        {
            .id = 3,
            .position = {270, 96},
            .title = "Blend",
            .meta = "mix",
            .inputs = kBlendIn,
            .inputCount = 3,
            .outputs = kBlendOut,
            .outputCount = 1,
            .bodyHeight = 28,
        },
        {.id = 4, .position = {510, 130}, .title = "Surface", .meta = "output", .inputs = kSurfaceIn, .inputCount = 3},
    };
    graph.wires = {
        {.id = graph.nextWireId++, .link = {.fromNode = 1, .fromPort = 1, .toNode = 3, .toPort = 1}},
        {.id = graph.nextWireId++, .link = {.fromNode = 2, .fromPort = 1, .toNode = 3, .toPort = 3}},
        {.id = graph.nextWireId++, .link = {.fromNode = 3, .fromPort = 1, .toNode = 4, .toPort = 1}},
    };
}

} // namespace

GalleryState::GalleryState() {
    inputs.prose = "Soft wrap breaks this paragraph at the field width, so the caret, the selection bands and Up and "
        "Down all follow the visual lines rather than the single line the text is. Double-click a word, "
        "triple-click the line, and hold the drag to grow the selection by that unit.";
    inputs.snippet = "vec3 n = normalize(worldNormal);\n"
        "float d = dot(n, lightDir);\n"
        "return baseColor * max(d, 0.0);";
    inputs.styled = prTextEditBufferCreate("Select some text and give it a style. Each style is a run of its own, and the "
        "caret, the selection and undo count through them all.");
    inputs.styledHistory = prTextEditHistoryCreate(inputs.styled);
    inputs.commands = {"build --release", "suite render css/flex-basis", "bench vg"};
    uint32_t key = 0;
    for (const TreeSeed& seed : kTreeSeeds) {
        TreeNode node;
        node.name = seed.name;
        node.meta = seed.meta;
        node.badge = seed.badge;
        node.depth = seed.depth;
        node.key = key++;
        node.icon = seed.icon;
        node.badgeTone = seed.badgeTone;
        node.hidden = seed.hidden;
        tree.nodes.push_back(node);
    }
    markChildren(tree.nodes);
    for (size_t i = 0; i < tree.nodes.size(); ++i)
        tree.nodes[i].open = tree.nodes[i].hasChildren && kTreeSeeds[i].open;
    chips = {"player", "physics", "no-lod", "shader:pbr"};
    seedCollections(collections);
    seedTimeline(timeline);
    seedGraph(graph);
}

GalleryState::~GalleryState() {
    prTextEditHistoryDestroy(inputs.styledHistory);
    prTextEditBufferDestroy(inputs.styled);
    prDrawImageRelease(picture);
}

const PrDrawImage* galleryPicture(GalleryState& state) {
    if (state.picture != nullptr)
        return state.picture;
    constexpr uint32_t kWidth = 512;
    constexpr uint32_t kHeight = 256;
    std::vector<uint8_t> pixels(kWidth * kHeight * 4);
    auto mix = [](float a, float b, float t) { return a + (b - a) * t; };
    for (uint32_t y = 0; y < kHeight; ++y) {
        for (uint32_t x = 0; x < kWidth; ++x) {
            float u = (float(x) + 0.5f) / kWidth;
            float v = (float(y) + 0.5f) / kHeight;
            // A sky from deep blue to a warm horizon, a sun toward the right, and two ridges of hills before it.
            float t = v / 0.7f;
            float color[3] = {mix(0.10f, 0.95f, t), mix(0.16f, 0.55f, t), mix(0.38f, 0.32f, t)};
            float dx = (u - 0.72f) * 2;
            float dy = v - 0.45f;
            if (dx * dx + dy * dy < 0.012f) {
                color[0] = 1.0f;
                color[1] = 0.86f;
                color[2] = 0.55f;
            }
            float far = 0.62f + 0.08f * sinF32(u * 9 + 1) + 0.04f * sinF32(u * 23);
            float near = 0.76f + 0.07f * sinF32(u * 5 + 3) + 0.03f * sinF32(u * 17 + 2);
            if (v > far) {
                color[0] = 0.22f;
                color[1] = 0.24f;
                color[2] = 0.36f;
            }
            if (v > near) {
                color[0] = 0.10f;
                color[1] = 0.16f;
                color[2] = 0.14f;
            }
            uint8_t* texel = &pixels[(size_t(y) * kWidth + x) * 4];
            for (uint32_t c = 0; c < 3; ++c)
                texel[c] = uint8_t(color[c] * 255 + 0.5f);
            texel[3] = 255;
        }
    }
    PrDrawImageDesc desc{
        PrPixelView{pixels.data(), kWidth * 4, {kWidth, kHeight}}, PrPixelFormat_Rgba8,
        PrDrawAlpha_Straight, PrColorSpace_Srgb,
    };
    const PrDrawImage* image = nullptr;
    if (prDrawImageCreate(&desc, &image) == PrStatus_Ok)
        state.picture = image;
    return state.picture;
}

const AssetRow* galleryAssets(uint32_t* count) {
    static const AssetRow kRows[] = {
        {"player_base.mesh", "Mesh", "1.2 MB", "12:04", 1258291, 1204, PrKitIconKind_Cube},
        {"env_diffuse.ktx2", "Texture", "8.4 MB", "11:58", 8808038, 1158, PrKitIconKind_Image},
        {"footsteps_grass_01.ogg", "Audio", "340 KB", "10:21", 348160, 1021, PrKitIconKind_Audio},
        {"level_01.pr", "Scene", "42 KB", "09:17", 43008, 917, PrKitIconKind_Folder},
        {"controller.lua", "Script", "2.1 KB", "Yesterday", 2150, 0, PrKitIconKind_Script},
        {"water_pbr.material", "Material", "620 B", "Yesterday", 620, 0, PrKitIconKind_Sphere},
    };
    *count = countOf(kRows);
    return kRows;
}

void sortAssetOrder(const AssetRow* assets, uint32_t count, AssetSort column, bool ascending, uint32_t* order) {
    for (uint32_t i = 0; i < count; ++i)
        order[i] = i;
    // An insertion sort keeps equal rows in their order.
    for (uint32_t i = 1; i < count; ++i) {
        uint32_t current = order[i];
        uint32_t at = i;
        while (at > 0 && assetLess(assets[current], assets[order[at - 1]], column, ascending)) {
            order[at] = order[at - 1];
            --at;
        }
        order[at] = current;
    }
}

const PrStringView* galleryComponentTypes(uint32_t* count) {
    static const PrStringView kTypes[] = {
        "Transform", "Mesh Renderer", "Collider",
        "Rigidbody", "Script",        "Audio Source",
        "Camera",
    };
    *count = countOf(kTypes);
    return kTypes;
}

const PrStringView* galleryAnimationClips(uint32_t* count) {
    static const char* const kNames[] = {
        "Idle",           "Idle Breathe",     "Idle Look Left",  "Idle Look Right", "Idle Scratch",
        "Idle Stretch",   "Idle Yawn",        "Idle Impatient",  "Idle Crouch",     "Idle Alert",
        "Walk Forward",   "Walk Back",        "Walk Left",       "Walk Right",      "Walk Slow",
        "Walk Injured",   "Walk Aim",         "Walk Crouch",     "Walk Turn Left",  "Walk Turn Right",
        "Run Forward",    "Run Back",         "Run Left",        "Run Right",       "Run Aim",
        "Run Turn Left",  "Run Turn Right",   "Sprint",          "Sprint Stop",     "Sprint Slide",
        "Jump Start",     "Jump Loop",        "Jump Land",       "Jump Land Hard",  "Jump Land Roll",
        "Fall Loop",      "Fall Recover",     "Vault Low",       "Vault High",      "Climb Ladder Up",
        "Climb Ladder Down", "Climb Ledge",   "Swim Idle",       "Swim Forward",    "Swim Dive",
        "Attack Light 1", "Attack Light 2",   "Attack Light 3",  "Attack Heavy",    "Attack Spin",
        "Block Start",    "Block Loop",       "Block Impact",    "Parry",           "Dodge Roll",
        "Dodge Step Back", "Hit React Front", "Hit React Back",  "Hit React Left",  "Hit React Right",
        "Death Front",    "Death Back",       "Emote Wave",      "Emote Point",     "Emote Salute",
        "Emote Sit",
    };
    static PrStringView clips[countOf(kNames)];
    if (clips[0].size == 0) {
        for (size_t i = 0; i < countOf(kNames); ++i)
            clips[i] = kNames[i];
    }
    *count = countOf(kNames);
    return clips;
}

const CommandItem* galleryCommands(uint32_t* count) {
    static const CommandItem kItems[] = {
        {"New Scene", "File", "Cmd N", PrKitIconKind_Folder},
        {"Open Scene...", "File", "Cmd O", PrKitIconKind_Folder},
        {"Save", "File", "Cmd S", PrKitIconKind_Folder},
        {"Undo", "Edit", "Cmd Z", PrKitIconKind_Undo},
        {"Duplicate Entity", "Edit", "Cmd D", PrKitIconKind_Script},
        {"Toggle Gizmos", "View", "Cmd ;", PrKitIconKind_Eye},
        {"Focus Selected", "View", "F", PrKitIconKind_Target},
        {"Create Empty", "Entity", "Shift Cmd N", PrKitIconKind_Cube},
        {"Add Mesh Renderer", "Entity", nullptr, PrKitIconKind_Cube},
        {"Build and Run", "Build", "Cmd B", PrKitIconKind_Play},
    };
    *count = countOf(kItems);
    return kItems;
}

const PrStringView* galleryAreas(uint32_t* count) {
    static const PrStringView kAreas[] = {
        "lib/app",    "lib/core",   "lib/draw",
        "lib/gpu",    "lib/image",  "lib/layout",
        "lib/ui",     "lib/studio/kit", "lib/html",
        "tools/suite",
    };
    *count = countOf(kAreas);
    return kAreas;
}

const PrStringView* galleryRevisions(uint32_t* count) {
    static const PrStringView kRevisions[] = {
        "main",    "origin/main", "shadow-atlas",
        "v0.4.1",  "9f3c1ad",     "2b70e44",
        "c81da09",
    };
    *count = countOf(kRevisions);
    return kRevisions;
}

void submitCommand(InputsState& inputs) {
    if (inputs.command.empty())
        return;
    inputs.commands.push_back(inputs.command);
    inputs.command.clear();
}

bool commandMatches(const CommandItem& item, const std::string& query) {
    return containsIgnoreCase(item.label, query) || containsIgnoreCase(item.category, query);
}

void undoBegin(UndoState& undo) {
    undo.opened = undo.values;
    undo.wrote = false;
    if (undo.history.empty()) {
        undo.history.push_back(undo.opened);
        undo.cursor = 0;
        return;
    }
    undo.history[undo.cursor] = undo.opened;
}

void undoCommit(UndoState& undo) {
    if (!undo.wrote)
        return;
    undo.history.resize(undo.cursor + 1);
    undo.history.push_back(undo.values);
    undo.cursor = uint32_t(undo.history.size()) - 1;
}

bool undoAvailable(const UndoState& undo) {
    return undo.cursor > 0;
}

bool redoAvailable(const UndoState& undo) {
    return size_t(undo.cursor) + 1 < undo.history.size();
}

bool undoStep(UndoState& undo, bool forward) {
    if (!(forward ? redoAvailable(undo) : undoAvailable(undo)))
        return false;
    undo.cursor = forward ? undo.cursor + 1 : undo.cursor - 1;
    undo.values = undo.history[undo.cursor];
    return true;
}

void moveTab(TabsState& tabs, uint32_t from, uint32_t to) {
    uint32_t count = uint32_t(countOf(tabs.panelOrder));
    if (from >= count || to >= count || from == to)
        return;
    uint32_t moved = tabs.panelOrder[from];
    if (from < to) {
        for (uint32_t i = from; i < to; ++i)
            tabs.panelOrder[i] = tabs.panelOrder[i + 1];
    } else {
        for (uint32_t i = from; i > to; --i)
            tabs.panelOrder[i] = tabs.panelOrder[i - 1];
    }
    tabs.panelOrder[to] = moved;
    if (tabs.panel == from)
        tabs.panel = to;
    else if (from < to && tabs.panel > from && tabs.panel <= to)
        --tabs.panel;
    else if (to < from && tabs.panel >= to && tabs.panel < from)
        ++tabs.panel;
}

TreeNode* findTreeNode(TreeState& tree, uint32_t key) {
    size_t index = indexOfKey(tree.nodes, key);
    return index < tree.nodes.size() ? &tree.nodes[index] : nullptr;
}

void moveTreeNode(TreeState& tree, uint32_t key, uint32_t targetKey, TreePlacement placement) {
    std::vector<TreeNode>& nodes = tree.nodes;
    size_t start = indexOfKey(nodes, key);
    if (start >= nodes.size() || key == targetKey)
        return;
    size_t end = subtreeEnd(nodes, start);
    for (size_t i = start; i < end; ++i) {
        if (nodes[i].key == targetKey)
            return;
    }
    std::vector<TreeNode> moved(nodes.begin() + long(start), nodes.begin() + long(end));
    nodes.erase(nodes.begin() + long(start), nodes.begin() + long(end));
    size_t at = indexOfKey(nodes, targetKey);
    uint32_t depth = placement == TreePlacement_Into ? nodes[at].depth + 1 : nodes[at].depth;
    int32_t shift = int32_t(depth) - int32_t(moved[0].depth);
    for (TreeNode& node : moved)
        node.depth = uint32_t(int32_t(node.depth) + shift);
    size_t insertAt = placement == TreePlacement_Into  ? at + 1 : placement == TreePlacement_After ? subtreeEnd(nodes, at) : at;
    nodes.insert(nodes.begin() + long(insertAt), moved.begin(), moved.end());
    markChildren(nodes);
    if (placement == TreePlacement_Into)
        nodes[at].open = true;
}

void treeOrder(TreeState& tree) {
    tree.rows.clear();
    tree.keys.clear();
    bool skipping = false;
    uint32_t skipDepth = 0;
    for (const TreeNode& node : tree.nodes) {
        if (skipping && node.depth > skipDepth)
            continue;
        skipping = node.hasChildren && !node.open;
        skipDepth = node.depth;
        tree.rows.push_back(PrKitTreeItem{.depth = node.depth, .hasChildren = node.hasChildren, .open = node.open});
        tree.keys.push_back(node.key);
    }
}

bool selectionHolds(const std::vector<uint32_t>& selection, uint32_t value) {
    for (uint32_t held : selection) {
        if (held == value)
            return true;
    }
    return false;
}

void selectionApply(std::vector<uint32_t>& selection, const PrKitSelectionEdit& edit, const uint32_t* keys) {
    if (edit.op == PrKitSelectionOp_None)
        return;
    if (edit.op == PrKitSelectionOp_Replace)
        selection.clear();
    for (uint32_t i = 0; i < edit.count; ++i) {
        uint32_t index = edit.first + i;
        uint32_t value = keys != nullptr ? keys[index] : index;
        bool held = false;
        for (size_t j = 0; j < selection.size(); ++j) {
            if (selection[j] != value)
                continue;
            held = true;
            if (edit.op == PrKitSelectionOp_Toggle)
                selection.erase(selection.begin() + long(j));
            break;
        }
        if (!held)
            selection.push_back(value);
    }
}

void collectionsTreeOrder(CollectionsState& collections) {
    collections.groupOpen.resize(kCollectionGroups, 0);
    collections.treeRows.clear();
    collections.treeKeys.clear();
    for (uint32_t group = 0; group < kCollectionGroups; ++group) {
        bool open = collections.groupOpen[group] != 0;
        collections.treeRows.push_back(PrKitTreeItem{.depth = 0, .hasChildren = true, .open = open});
        collections.treeKeys.push_back(group << 16);
        if (!open)
            continue;
        for (uint32_t child = 0; child < kCollectionGroupSize; ++child) {
            collections.treeRows.push_back(PrKitTreeItem{.depth = 1});
            collections.treeKeys.push_back((group << 16) | (child + 1));
        }
    }
}

uint32_t collectionGroupOf(uint32_t key) {
    return key >> 16;
}

bool collectionAssetOf(uint32_t key, uint32_t* index) {
    uint32_t child = key & 0xffff;
    if (child == 0)
        return false;
    *index = collectionGroupOf(key) * kCollectionGroupSize + child - 1;
    return true;
}

} // namespace gallery
