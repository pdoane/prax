// The editor's model (editor_state.h).

#include "examples/editor/editor_state.h"

#include <stdio.h>

namespace editor {

namespace {

// An entity of the level as it opens: its place in the tree, and where it stands, in meters.
struct LevelEntity {
    const char* name;
    const char* type;
    uint32_t depth;
    PrKitIconKind icon;
    float x;
    float z;
    bool open;
};

constexpr LevelEntity kLevel[] = {
    {"Environment", "Group", 0, PrKitIconKind_Folder, 0, 0, true},
    {"Terrain", "Mesh", 1, PrKitIconKind_Terrain, 0, 0, true},
    {"Pond", "Mesh", 1, PrKitIconKind_Sphere, -9, 6, true},
    {"Sun", "Light", 1, PrKitIconKind_Light, 12, -11, true},
    {"Player", "Entity", 0, PrKitIconKind_Cube, -2, 3, true},
    {"Camera Rig", "Camera", 1, PrKitIconKind_Camera, -2, 7, true},
    {"Controller", "Script", 1, PrKitIconKind_Script, -2, 3, true},
    {"Enemies", "Group", 0, PrKitIconKind_Folder, 0, 0, true},
    {"Grunt_01", "Entity", 1, PrKitIconKind_Cube, 7, -4, true},
    {"Grunt_02", "Entity", 1, PrKitIconKind_Cube, 9, -1, true},
    {"Grunt_03", "Entity", 1, PrKitIconKind_Cube, 5, -7, true},
    {"Props", "Group", 0, PrKitIconKind_Folder, 0, 0, false},
    {"Crate_A", "Mesh", 1, PrKitIconKind_Cube, -6, -3, true},
    {"Crate_B", "Mesh", 1, PrKitIconKind_Cube, -5, -4.5f, true},
    {"Barrel", "Mesh", 1, PrKitIconKind_Cube, -7.5f, -5, true},
    {"Lamp Post", "Light", 1, PrKitIconKind_Light, 2, -2, true},
};

struct LevelLog {
    PrKitLogLevel level;
    const char* message;
    const char* source;
};

constexpr LevelLog kLoadLog[] = {
    {PrKitLogLevel_Info, "Opened Level_01 with 16 entities", "Level"},
    {PrKitLogLevel_Info, "Compiled 12 shader variants", "Shaders"},
    {PrKitLogLevel_Warning, "Texture env_normal is not a power of two", "Assets"},
    {PrKitLogLevel_Warning, "Rigidbody on Player has no collider", "Physics"},
    {PrKitLogLevel_Error, "Script enemy_ai: undefined symbol patrol_radius", "Scripting"},
};

// Marks the entities that head a subtree, from the depths of those that follow them.
void markChildren(std::vector<Entity>& entities) {
    for (size_t i = 0; i < entities.size(); ++i)
        entities[i].hasChildren = i + 1 < entities.size() && entities[i + 1].depth > entities[i].depth;
}

// Returns one past the last descendant of the entity at `start`.
size_t subtreeEnd(const std::vector<Entity>& entities, size_t start) {
    size_t end = start + 1;
    while (end < entities.size() && entities[end].depth > entities[start].depth)
        ++end;
    return end;
}

size_t indexOf(const std::vector<Entity>& entities, EntityId id) {
    for (size_t i = 0; i < entities.size(); ++i) {
        if (entities[i].id == id)
            return i;
    }
    return entities.size();
}

} // namespace

void EditorState::loadLevel() {
    levelName = "Level_01";
    entities.clear();
    for (const LevelEntity& level : kLevel) {
        Entity entity;
        entity.id = nextId++;
        entity.depth = level.depth;
        entity.name = level.name;
        entity.type = level.type;
        entity.icon = level.icon;
        entity.open = level.open;
        entity.mesh = "player_base.mesh";
        entity.position = {level.x, 0.0f, level.z};
        entities.push_back(entity);
    }
    markChildren(entities);
    selected = 5;
    logs.clear();
    for (const LevelLog& log : kLoadLog)
        addLog(log.level, log.message, log.source);
}

Entity* PR_NULLABLE EditorState::find(EntityId id) {
    size_t index = indexOf(entities, id);
    return index < entities.size() ? &entities[index] : nullptr;
}

void EditorState::addEntity() {
    Entity entity;
    entity.id = nextId++;
    entity.name = "Entity_" + std::to_string(entity.id);
    entity.type = "Entity";
    entities.push_back(entity);
    markChildren(entities);
    selected = entity.id;
    addLog(PrKitLogLevel_Info, "Added " + entity.name, "Scene");
}

void EditorState::removeEntity(EntityId id) {
    size_t start = indexOf(entities, id);
    if (start >= entities.size())
        return;
    std::string name = entities[start].name;
    entities.erase(entities.begin() + long(start), entities.begin() + long(subtreeEnd(entities, start)));
    markChildren(entities);
    if (selected == id)
        selected = 0;
    if (lockedEntity == id) {
        lockedEntity = 0;
        inspectorLocked = false;
    }
    addLog(PrKitLogLevel_Info, "Removed " + name, "Scene");
}

void EditorState::moveEntity(EntityId id, EntityId target, TreePlacement placement) {
    size_t start = indexOf(entities, id);
    if (start >= entities.size() || id == target)
        return;
    size_t end = subtreeEnd(entities, start);
    // An entity cannot move into its own subtree.
    for (size_t i = start; i < end; ++i) {
        if (entities[i].id == target)
            return;
    }
    std::vector<Entity> moved(entities.begin() + long(start), entities.begin() + long(end));
    entities.erase(entities.begin() + long(start), entities.begin() + long(end));
    size_t at = indexOf(entities, target);
    uint32_t depth = placement == TreePlacement_Into ? entities[at].depth + 1 : entities[at].depth;
    int32_t shift = int32_t(depth) - int32_t(moved[0].depth);
    for (Entity& entity : moved)
        entity.depth = uint32_t(int32_t(entity.depth) + shift);
    size_t insertAt = at;
    if (placement == TreePlacement_Into) {
        insertAt = at + 1;
        entities[at].open = true;
    } else if (placement == TreePlacement_After) {
        insertAt = subtreeEnd(entities, at);
    }
    entities.insert(entities.begin() + long(insertAt), moved.begin(), moved.end());
    markChildren(entities);
}

void EditorState::addLog(PrKitLogLevel level, const std::string& message, const std::string& source) {
    // The log's clock starts at the level's opening, a second for each line.
    uint32_t second = uint32_t(logs.size());
    char time[16];
    snprintf(time, sizeof time, "00:00:%02u", second % 60);
    logs.push_back({time, message, source, level});
}

void EditorState::clearLogs() {
    logs.clear();
}

bool EditorState::shows(PrKitLogLevel level) const {
    switch (level) {
        case PrKitLogLevel_Warning: return showWarnings;
        case PrKitLogLevel_Error: return showErrors;
        default: return showInfo;
    }
}

} // namespace editor
