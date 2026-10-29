// The editor's model: a scene of entities, a log, and the choices its panels show.

#pragma once

#include "pr/color.h"
#include "pr/kit_collection.h"
#include "pr/kit_data.h"
#include "pr/kit_dock.h"
#include "pr/kit_icon.h"

#include <stdint.h>
#include <string>
#include <vector>

PR_ASSUME_NONNULL_BEGIN

namespace editor {

struct Hud;

// An entity of the scene, numbered from 1.
typedef uint32_t EntityId;

// The active tool for manipulating the viewport.
enum {
    Tool_Select,
    Tool_Move,
    Tool_Rotate,
    Tool_Scale,
};
typedef uint8_t Tool;

// The tab the bottom panel shows.
enum {
    ConsoleTab_Console,
    ConsoleTab_AssetBrowser,
    ConsoleTab_Timeline,
    ConsoleTab_Profiler,
};
typedef uint8_t ConsoleTab;

// A three-component value the inspector edits.
struct Vector3 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

// One node of the scene tree, flattened in preorder with its depth.
struct SceneEntity {
    EntityId id = 0;
    uint32_t depth = 0;
    std::string name;
    std::string typeLabel;
    std::string meshAsset;
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

// One console line.
struct LogEntry {
    std::string time;
    std::string message;
    std::string source;
    PrKitLogLevel level = PrKitLogLevel_Info;
};

// Where an entity is placed relative to the drop target.
enum {
    TreePlacement_Before,
    TreePlacement_Into,
    TreePlacement_After,
};
typedef uint8_t TreePlacement;

// The latest and smoothed frame times, in milliseconds.
struct FrameTiming {
    float cpuMs = 0.0f;       // latest CPU time
    float gpuMs = 0.0f;       // latest GPU time
    float smoothCpuMs = 0.0f; // smoothed CPU time
    float smoothGpuMs = 0.0f; // smoothed GPU time
    uint32_t frames = 0;

    // Adds one frame. The first frame sets the means.
    void note(float cpu, float gpu);
};

// The editor's mutable model.
struct EditorState {
    FrameTiming timing;
    std::vector<SceneEntity> nodes;
    std::vector<LogEntry> logs;
    std::vector<const LogEntry*> visibleLogs;
    std::vector<EntityId> sceneRows;       // the entities that the scene tree shows, in order
    std::vector<PrKitTreeItem> sceneItems; // by scene row
    PrKitSelection sceneCursor = {kPrKitRowNone, kPrKitRowNone};
    PrKitDock* dock = nullptr;            // the dock's model, created by the first build
    Hud* hud = nullptr;                   // the viewport's HUD, for the host's context (editorAttach)

    EditorState() = default;
    ~EditorState() { prKitDockDestroy(dock); }
    EditorState(const EditorState&) = delete;
    EditorState& operator=(const EditorState&) = delete;
    EntityId selected = 0;
    EntityId nextId = 1;
    EntityId lockedEntity = 0;
    std::string sceneFilter;
    std::string paletteQuery;
    uint32_t paletteActive = 0;
    uint32_t selectedAsset = 0;
    uint32_t renderMode = 0;
    uint32_t lighting = 0;
    uint32_t space = 0;
    uint32_t viewportTab = 0;
    Tool tool = Tool_Select;
    ConsoleTab consoleTab = ConsoleTab_Console;
    float drag = 0.35f;
    float scriptSpeed = 6.5f;
    float scriptJump = 1.2f;
    float unboundMass = 0.0f;
    PrColor tint = {0.85f, 0.62f, 0.32f, 1.0f};
    bool inspectorLocked = false;
    bool openTransform = true;
    bool openMesh = true;
    bool openRigidbody = true;
    bool openScript = false;
    bool grid = true;
    bool snap = false;
    bool playing = false;
    uint32_t edits = 0;                   // the edit gestures that the editor's controls committed
    bool paletteOpen = false;
    bool paletteJustOpened = false;
    bool showInfo = true;
    bool showWarn = true;
    bool showError = true;

    // Opens the level: fills the scene with its entities, and the log with what loading it reported.
    void loadLevel();

    SceneEntity* find(EntityId id);
    void addEntity();
    void removeEntity(EntityId id);
    void moveEntity(EntityId id, EntityId target, TreePlacement placement);
    void toggleOpen(EntityId id);
    void toggleHidden(EntityId id);
    void addLog(PrKitLogLevel level, const std::string& message, const std::string& source);
    void clearLogs();
    void toggleLevel(PrKitLogLevel level);
    void projectLogs();
};

// Returns the entity that the selection names, or null.
SceneEntity* selectedEntity(EditorState& editor);

// One command in the palette.
struct CommandItem {
    const char* label;    // display name
    const char* category; // search group
    const char* shortcut; // the chord that activates it
    PrKitIconKind icon;
};

// Returns the command palette's entries in display order.
const CommandItem* editorCommands(uint32_t* count);

// Returns true if `query` matches the command's label or category.
bool commandMatches(const CommandItem& item, const std::string& query);

// Returns true if `text` contains `needle`, ignoring case.
bool containsIgnoreCase(const std::string& text, const std::string& needle);

} // namespace editor

PR_ASSUME_NONNULL_END
