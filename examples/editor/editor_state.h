// The editor's model: a level of entities laid out on a ground plane, the console's log, and the choices that the
// panels show. The view reads and writes it every round.

#pragma once

#include "pr/array.h"
#include "pr/color.h"
#include "pr/kit_collection.h"
#include "pr/kit_data.h"
#include "pr/kit_dock.h"
#include "pr/kit_icon.h"
#include "pr/string.h"

#include <stdint.h>

PR_ASSUME_NONNULL_BEGIN

namespace editor {

using namespace pr;

struct Hud;

// An entity of the level, numbered from 1.
typedef uint32_t EntityId;

// The tool that the viewport's pointer uses.
enum {
    Tool_Select,
    Tool_Move,
    Tool_Rotate,
    Tool_Scale,
};
typedef uint8_t Tool;

// How the viewport draws the entities.
enum {
    RenderMode_Shaded,
    RenderMode_Flat,
    RenderMode_Wireframe,
    RenderMode_Count,
};
typedef uint8_t RenderMode;

// Three components that the inspector edits.
struct Vector3 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

// An entity of the scene tree, flattened in preorder with its depth. Its position is in meters on the ground plane, x
// to the east and z to the south, which the viewport shows from above.
struct Entity {
    EntityId id = 0;
    uint32_t depth = 0;
    String name;
    String type;
    String mesh;
    PrKitIconKind icon = PrKitIconKind_Cube;
    Vector3 position;
    Vector3 rotation;
    Vector3 scale = {1.0f, 1.0f, 1.0f};
    float mass = 1.0f;
    bool hasChildren = false;
    bool open = true;
    bool hidden = false;
    bool enabled = true;
    bool castShadows = true;
    bool receiveShadows = true;
    bool gravity = true;
};

// A line of the console.
struct LogEntry {
    String time;
    String message;
    String source;
    PrKitLogLevel level = PrKitLogLevel_Info;
};

// Where an entity that is dropped on another goes.
enum {
    TreePlacement_Before,
    TreePlacement_Into,
    TreePlacement_After,
};
typedef uint8_t TreePlacement;

// The editor's model.
struct EditorState {
    EditorState() = default;
    ~EditorState() { prKitDockDestroy(dock); }
    EditorState(const EditorState&) = delete;
    EditorState& operator=(const EditorState&) = delete;

    // The level, its selection, and the inspector's lock on one entity.
    String levelName;
    Array<Entity> entities;
    EntityId selected = 0;
    EntityId nextId = 1;
    bool inspectorLocked = false;
    EntityId lockedEntity = 0;

    // The scene tree: its filter, the rows that it shows, and its cursor.
    String sceneFilter;
    Array<EntityId> sceneRows;
    Array<PrKitTreeItem> sceneItems;
    PrKitSelection sceneCursor = {kPrKitRowNone, kPrKitRowNone};

    // The console's log, and the levels that it shows.
    Array<LogEntry> logs;
    bool showInfo = true;
    bool showWarnings = true;
    bool showErrors = true;

    // The viewport and the toolbar.
    Tool tool = Tool_Select;
    RenderMode renderMode = RenderMode_Shaded;
    uint32_t space = 0;             // 0 for local coordinates, 1 for world
    bool grid = true;
    bool snap = false;
    bool playing = false;

    // The inspector's groups that are open, and the values that the components hold.
    bool openTransform = true;
    bool openMesh = true;
    bool openRigidbody = true;
    bool openScript = false;
    PrColor tint = {0.85f, 0.62f, 0.32f, 1.0f};
    uint32_t lighting = 0;
    float drag = 0.35f;
    float scriptSpeed = 6.5f;
    float scriptJump = 1.2f;

    uint32_t selectedAsset = 0;
    bool paletteOpen = false;
    uint32_t edits = 0;             // the edit gestures that the editor's controls committed, of either library

    PrKitDock* PR_NULLABLE dock = nullptr;  // the dock's layout, made by the first round
    Hud* PR_NULLABLE hud = nullptr;         // the viewport's HUD, which the context that the view runs in owns

    // Opens the level: its entities, and the log of loading it.
    void loadLevel();

    Entity* PR_NULLABLE find(EntityId id);
    void addEntity();
    void removeEntity(EntityId id);
    void moveEntity(EntityId id, EntityId target, TreePlacement placement);
    void addLog(PrKitLogLevel level, PrStringView message, PrStringView source);
    void clearLogs();

    // Returns true if the console shows lines of `level`.
    bool shows(PrKitLogLevel level) const;
};

} // namespace editor

PR_ASSUME_NONNULL_END
