#include "tools/bench/editor_bench/editor_state.h"

#include "pr/count_of.h"

#include <ctype.h>

namespace editor {

using namespace pr;

namespace {

struct LevelEntity {
    const char* name;
    const char* type;
    uint32_t depth;
    PrKitIconKind icon;
    bool open;
};

constexpr LevelEntity kLevelEntities[] = {
    {"Level_01", "Scene", 0, PrKitIconKind_Folder, true},
    {"Environment", "Group", 1, PrKitIconKind_Folder, true},
    {"Terrain", "Mesh", 2, PrKitIconKind_Terrain, true},
    {"Water", "Mesh", 2, PrKitIconKind_Sphere, true},
    {"Sun", "Light", 2, PrKitIconKind_Light, true},
    {"Sky Probe", "Light", 2, PrKitIconKind_Light, true},
    {"Player", "Entity", 1, PrKitIconKind_Cube, true},
    {"Camera Rig", "Camera", 2, PrKitIconKind_Camera, true},
    {"Main Camera", "Camera", 3, PrKitIconKind_Camera, true},
    {"Controller", "Script", 2, PrKitIconKind_Script, true},
    {"Footsteps", "Audio", 2, PrKitIconKind_Audio, true},
    {"Enemies", "Group", 1, PrKitIconKind_Folder, false},
    {"Grunt_01", "Entity", 2, PrKitIconKind_Cube, true},
    {"Grunt_02", "Entity", 2, PrKitIconKind_Cube, true},
    {"Grunt_03", "Entity", 2, PrKitIconKind_Cube, true},
    {"Props", "Group", 1, PrKitIconKind_Folder, true},
    {"Crate_A", "Mesh", 2, PrKitIconKind_Cube, true},
    {"Crate_B", "Mesh", 2, PrKitIconKind_Cube, true},
    {"Barrel", "Mesh", 2, PrKitIconKind_Cube, true},
    {"Lamp Post", "Mesh", 2, PrKitIconKind_Light, true},
    {"UI", "Group", 1, PrKitIconKind_Folder, true},
    {"HUD", "Canvas", 2, PrKitIconKind_Image, true},
    {"Minimap", "Canvas", 2, PrKitIconKind_Image, true},
};

struct LevelLog {
    PrKitLogLevel level;
    const char* time;
    const char* message;
    const char* source;
};

constexpr LevelLog kLevelLogs[] = {
    {PrKitLogLevel_Info, "14:02:11", "Scene Level_01 loaded in 412 ms", "SceneManager"},
    {PrKitLogLevel_Info, "14:02:11", "Compiled 12 shader variants", "Shaders"},
    {PrKitLogLevel_Warning, "14:02:12", "Texture env_normal is not power of two", "Assets"},
    {PrKitLogLevel_Info, "14:02:12", "Nav mesh baked: 1,204 polygons", "Navigation"},
    {PrKitLogLevel_Warning, "14:02:14", "Rigidbody on Player has no collider", "Physics"},
    {PrKitLogLevel_Error, "14:02:15", "Script enemy_ai: undefined symbol patrol_radius", "Scripting"},
    {PrKitLogLevel_Info, "14:02:16", "Play mode ready", "Editor"},
};

// Marks which entities head a subtree, from the depths of the entities that follow them.
void markChildren(std::vector<SceneEntity>& nodes) {
    for (size_t i = 0; i < nodes.size(); ++i) {
        nodes[i].hasChildren = i + 1 < nodes.size() && nodes[i + 1].depth > nodes[i].depth;
    }
}

// Returns one past the last descendant of the entity at `start`.
size_t subtreeEnd(const std::vector<SceneEntity>& nodes, size_t start) {
    size_t end = start + 1;
    while (end < nodes.size() && nodes[end].depth > nodes[start].depth) {
        ++end;
    }
    return end;
}

size_t indexOf(const std::vector<SceneEntity>& nodes, EntityId id) {
    for (size_t i = 0; i < nodes.size(); ++i) {
        if (nodes[i].id == id) {
            return i;
        }
    }
    return nodes.size();
}

} // namespace

void FrameTiming::note(float cpu, float gpu) {
    cpuMs = cpu;
    gpuMs = gpu;
    if (frames == 0) {
        smoothCpuMs = cpu;
        smoothGpuMs = gpu;
    } else {
        smoothCpuMs += (cpu - smoothCpuMs) * 0.05f;
        smoothGpuMs += (gpu - smoothGpuMs) * 0.05f;
    }
    ++frames;
}

void EditorState::loadLevel() {
    nodes.clear();
    for (const LevelEntity& entity : kLevelEntities) {
        SceneEntity node;
        node.id = nextId++;
        node.depth = entity.depth;
        node.name = entity.name;
        node.typeLabel = entity.type;
        node.icon = entity.icon;
        node.open = entity.open;
        node.meshAsset = "player_base.mesh";
        node.position = {float(node.id) * 0.5f, 0.0f, float(node.id) * -0.25f};
        nodes.push_back(node);
    }
    markChildren(nodes);
    selected = 7;
    logs.clear();
    for (const LevelLog& log : kLevelLogs) {
        logs.push_back({log.time, log.message, log.source, log.level});
    }
    projectLogs();
}

SceneEntity* EditorState::find(EntityId id) {
    const size_t index = indexOf(nodes, id);
    return index < nodes.size() ? &nodes[index] : nullptr;
}

void EditorState::addEntity() {
    SceneEntity node;
    node.id = nextId++;
    node.name = "Entity_" + std::to_string(node.id);
    node.typeLabel = "Entity";
    nodes.push_back(node);
    markChildren(nodes);
    selected = node.id;
    addLog(PrKitLogLevel_Info, "Added " + node.name, "Scene");
}

void EditorState::removeEntity(EntityId id) {
    const size_t start = indexOf(nodes, id);
    if (start >= nodes.size()) {
        return;
    }
    const std::string name = nodes[start].name;
    nodes.erase(nodes.begin() + long(start), nodes.begin() + long(subtreeEnd(nodes, start)));
    markChildren(nodes);
    if (selected == id) {
        selected = 0;
    }
    if (lockedEntity == id) {
        lockedEntity = 0;
        inspectorLocked = false;
    }
    addLog(PrKitLogLevel_Info, "Removed " + name, "Scene");
}

void EditorState::moveEntity(EntityId id, EntityId target, TreePlacement placement) {
    const size_t start = indexOf(nodes, id);
    if (start >= nodes.size() || id == target) {
        return;
    }
    const size_t end = subtreeEnd(nodes, start);
    for (size_t i = start; i < end; ++i) {
        if (nodes[i].id == target) {
            return;
        }
    }
    std::vector<SceneEntity> moved(nodes.begin() + long(start), nodes.begin() + long(end));
    nodes.erase(nodes.begin() + long(start), nodes.begin() + long(end));
    const size_t at = indexOf(nodes, target);
    if (at >= nodes.size()) {
        return;
    }
    const uint32_t depth = placement == TreePlacement_Into ? nodes[at].depth + 1 : nodes[at].depth;
    const int32_t shift = int32_t(depth) - int32_t(moved[0].depth);
    for (SceneEntity& node : moved) {
        node.depth = uint32_t(int32_t(node.depth) + shift);
    }
    size_t insertAt = at;
    if (placement == TreePlacement_Into) {
        insertAt = at + 1;
        nodes[at].open = true;
    } else if (placement == TreePlacement_After) {
        insertAt = subtreeEnd(nodes, at);
    }
    nodes.insert(nodes.begin() + long(insertAt), moved.begin(), moved.end());
    markChildren(nodes);
}

void EditorState::toggleOpen(EntityId id) {
    if (SceneEntity* node = find(id)) {
        node->open = !node->open;
    }
}

void EditorState::toggleHidden(EntityId id) {
    if (SceneEntity* node = find(id)) {
        node->hidden = !node->hidden;
    }
}

void EditorState::addLog(PrKitLogLevel level, const std::string& message, const std::string& source) {
    logs.push_back({"14:02:" + std::to_string(20 + logs.size() % 40), message, source, level});
    projectLogs();
}

void EditorState::clearLogs() {
    logs.clear();
    projectLogs();
}

void EditorState::toggleLevel(PrKitLogLevel level) {
    switch (level) {
        case PrKitLogLevel_Info:
            showInfo = !showInfo;
            break;
        case PrKitLogLevel_Warning:
            showWarn = !showWarn;
            break;
        case PrKitLogLevel_Error:
            showError = !showError;
            break;
    }
    projectLogs();
}

void EditorState::projectLogs() {
    visibleLogs.clear();
    for (const LogEntry& entry : logs) {
        const bool shown = entry.level == PrKitLogLevel_Info      ? showInfo
                           : entry.level == PrKitLogLevel_Warning ? showWarn
                                                                  : showError;
        if (shown) {
            visibleLogs.push_back(&entry);
        }
    }
}

SceneEntity* selectedEntity(EditorState& editor) {
    return editor.selected != 0 ? editor.find(editor.selected) : nullptr;
}

const CommandItem* editorCommands(uint32_t* count) {
    static const CommandItem items[] = {
        {"New Scene", "File", "Cmd N", PrKitIconKind_Folder},
        {"Open Scene...", "File", "Cmd O", PrKitIconKind_Folder},
        {"Save", "File", "Cmd S", PrKitIconKind_Folder},
        {"Undo", "Edit", "Cmd Z", PrKitIconKind_Undo},
        {"Duplicate Entity", "Edit", "Cmd D", PrKitIconKind_Script},
        {"Toggle Grid", "View", "Cmd '", PrKitIconKind_Grid},
        {"Focus Selected", "View", "F", PrKitIconKind_Target},
        {"Create Empty", "Entity", "Shift Cmd N", PrKitIconKind_Cube},
        {"Add Mesh Renderer", "Entity", nullptr, PrKitIconKind_Cube},
        {"Build and Run", "Build", "Cmd B", PrKitIconKind_Play},
    };
    *count = countOf(items);
    return items;
}

bool commandMatches(const CommandItem& item, const std::string& query) {
    return containsIgnoreCase(item.label, query) || containsIgnoreCase(item.category, query);
}

bool containsIgnoreCase(const std::string& text, const std::string& needle) {
    if (needle.empty()) {
        return true;
    }
    if (needle.size() > text.size()) {
        return false;
    }
    for (size_t i = 0; i + needle.size() <= text.size(); ++i) {
        size_t j = 0;
        while (j < needle.size() && tolower(text[i + j]) == tolower(needle[j])) {
            ++j;
        }
        if (j == needle.size()) {
            return true;
        }
    }
    return false;
}

} // namespace editor
