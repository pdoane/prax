// Alchemy's state: the server it talks to, the parameters of the next run, the runs in flight, and the gallery. The
// panels, the commands and the key bindings all read and change it, and the frame loop pumps the server's events into
// it before each build.

#pragma once

#include "pr/array.h"
#include "pr/base.h"
#include "pr/compress.h"
#include "pr/draw_image.h"
#include "pr/hash.h"
#include "pr/hash_key.h"
#include "pr/image.h"
#include "pr/lock.h"
#include "pr/map.h"
#include "pr/mutex.h"
#include "pr/random.h"
#include "pr/string.h"
#include "pr/string_view.h"
#include "pr/task.h"

#include "apps/alchemy/comfy_client.h"
#include "apps/alchemy/dimensions.h"
#include "apps/alchemy/gallery.h"
#include "apps/alchemy/image_cache.h"
#include "apps/alchemy/models.h"
#include "apps/alchemy/provenance.h"
#include "apps/alchemy/recipes.h"

#include <stdint.h>

PR_ASSUME_NONNULL_BEGIN

namespace alchemy {

using namespace pr;

// What a run does. Generate runs text-to-image, or image-to-image if a source image is held. The other modes run over
// the source image.
enum {
    OperationMode_Generate,
    OperationMode_Upscale,
    OperationMode_Outpaint,
    OperationMode_Detail,
    OperationMode_Count,
};
typedef uint8_t OperationMode;

// Return the mode's name as commands and records spell it, and the mode that a name names or `fallback`.
PrStringView modeName(OperationMode mode);
OperationMode parseMode(PrStringView name, OperationMode fallback);

// Returns true if the mode runs over a source image, and true if it samples, reading the model, the prompts and the
// sampler's settings.
bool modeNeedsSource(OperationMode mode);
bool modeSamples(OperationMode mode);

// The server's connection, as the status bar shows it.
enum {
    ConnectionState_Disconnected,
    ConnectionState_Connecting,
    ConnectionState_Connected,
};
typedef uint8_t ConnectionState;

// An adapter that a run chains onto its checkpoint, by its file's name on the server.
struct LoraEntry {
    String name;
    double weight = 1.0;
};

// The inputs that the sampling modes share, and those of Generate. Models are held by the names that the server knows.
// The source image stays held until a command replaces or clears it.
struct GenerateParams {
    String prompt;
    String negativePrompt;
    String sourcePath;
    Array<LoraEntry> loras;
    String checkpoint;
    String sampler;
    String scheduler;
    double strength = 0.75;     // how far an image-to-image run may move from its source
    double cfg = 7.0;
    uint64_t seed = 0;
    uint32_t steps = 20;
    uint32_t batch = 1;
    bool seedLocked = false;    // the next run keeps the seed rather than drawing one
};

// The inputs of the other modes.
struct UpscaleParams {
    String model;
    double factor = 2.0;
};
struct OutpaintParams {
    OutpaintDirection direction = OutpaintDirection_Right;
    uint32_t amount = 256;
    uint32_t crossfade = 64;
    uint32_t growMask = 8;
    uint32_t context = 512;
};
struct DetailParams {
    String detector;
};

// Where a run stands on the server.
enum {
    JobState_Pending,
    JobState_Running,
    JobState_Done,
    JobState_Failed,
};
typedef uint8_t JobState;

// A prompt that the server queued, and the record that its images are saved with.
struct Job {
    String promptId;
    JobState state = JobState_Pending;
    uint32_t progressValue = 0;
    uint32_t progressMax = 0;
    String node;           // the stage that the run reached, by its node's id
    String error;
    ProvenanceRecord record;

    // The size of the image that the run produces, or zero if only the finished image says. A latent preview is shown
    // at this size, so that the finished image lands where the preview stood.
    DimensionSize output;
};

// A run on its way to the server: what it asked for, and the bytes of its source image, which its record continues.
struct Submission {
    OperationMode mode = OperationMode_Generate;
    GenerateParams sampling;
    UpscaleParams upscale;
    OutpaintParams outpaint;
    DetailParams detail;
    DimensionSize size;
    DimensionSize sourceSize;
    Array<uint8_t> sourceBytes;
    String sourceName;     // the name that the server stored the source under
};

// An event that names a prompt whose submission has not been answered yet, which the server's stream can deliver before
// the submission's reply. It is held until the reply comes, or for a few seconds.
struct HeldEvent {
    ComfyEvent event;
    double age = 0;
};

// The images that the session visited, in order, which Back and Forward walk without recording the walk. A visit is a
// path, since a rescan reorders the entries.
struct VisitHistory {
    Array<String> visits;
    uint32_t position = 0;
};

// A model file as a lookup names it: its folder and its name, which the lookup does not copy.
struct ModelKeyView {
    ModelFolder folder;
    PrStringView name;
};

// A model file by its folder and its name, which the survey files its architecture under.
struct ModelKey {
    // A table keyed by model files looks them up by their views (pr/hash_key.h).
    using Lookup = ModelKeyView;

    ModelFolder folder = 0;
    String name;

    ModelKey() = default;
    explicit ModelKey(const ModelKeyView& key) : folder(key.folder), name(key.name) {}
    operator ModelKeyView() const { return ModelKeyView{folder, name}; }

    friend bool operator==(const ModelKey& a, const ModelKeyView& b) { return a.folder == b.folder && a.name == b.name; }
};

// Return the hash of a model file's key, which is the same for a key and its view: its name's bytes, seeded by its
// folder.
inline uint32_t hashKey(const ModelKeyView& key) {
    uint64_t hash = prHashBytes(key.name.data, key.name.size, key.folder);
    return uint32_t(hash ^ (hash >> 32));
}
inline uint32_t hashKey(const ModelKey& key) {
    return hashKey(ModelKeyView{key.folder, key.name});
}

// The architecture that a model file states, as a survey task reads it.
struct SurveyedModel {
    ModelFolder folder;
    String name;
    ModelArchitecture architecture;
};

// The architectures that model files state, by folder and name, read on the task pool.
struct ModelSurvey {
    ModelSurvey() { prMutexInit(&mutex); }
    ~ModelSurvey() { prMutexReset(&mutex); }
    ModelSurvey(const ModelSurvey&) = delete;
    ModelSurvey& operator=(const ModelSurvey&) = delete;

    Map<ModelKey, ModelArchitecture> known;
    PrMutex mutex;
    Array<SurveyedModel> landed;
    uint32_t generation = 0;    // the count of catalogs, so that a survey of an older one stops
};

// An import into the gallery: the image that it would add, which a paste stages for the dialog to show, and what the
// last paste found if it found no image.
struct ImportPrompt {
    bool open = false;
    const PrDrawImage* PR_NULLABLE image = nullptr;
    String message;
};

// The groups of an operation's inputs that the record's verbs carry into the parameters.
enum {
    UseWhat_Prompt,
    UseWhat_Negative,
    UseWhat_PromptLine,     // a line of the prompt, which joins the prompt being written
    UseWhat_NegativeLine,
    UseWhat_Model,
    UseWhat_Loras,
    UseWhat_Sampling,
    UseWhat_Aspect,
    UseWhat_Seed,
    UseWhat_Strength,
    UseWhat_Upscale,
    UseWhat_Outpaint,
    UseWhat_Detector,
    UseWhat_Source,
    UseWhat_All,
    UseWhat_Count,
};
typedef uint8_t UseWhat;

// Returns the group that `name` names as commands spell it, or Count if it names none.
UseWhat parseUseWhat(PrStringView name);

// The state of the application.
struct AppState {
    // The services, which main() owns.
    ComfyClient* client = nullptr;
    ImageCache* images = nullptr;
    PrImageCodec* codec = nullptr;
    PrCompressCodec* compress = nullptr;
    PrTaskPool* pool = nullptr;

    // The server.
    String server = "http://127.0.0.1:8188";
    String connectedServer;    // the address that the last connection went to
    ConnectionState connection = ConnectionState_Disconnected;
    uint32_t queueRemaining = 0;
    String status;
    ComfyCatalog catalog;
    bool managerPresent = false;
    ModelSurvey survey;

    // The next run.
    OperationMode mode = OperationMode_Generate;
    GenerateParams params;
    Dimensions dims;
    UpscaleParams upscale;
    OutpaintParams outpaint;
    DetailParams detail;

    // The runs on their way and in flight, and the events that wait for them. Requests to the server carry tags, by
    // which their replies find the submission or the request that asked.
    uint64_t nextTag = 1;
    Map<uint64_t, Submission> uploading;
    Map<uint64_t, Submission> submitting;
    Array<Job> jobs;
    Array<HeldEvent> held;
    uint64_t catalogTag = 0;
    uint64_t managerTag = 0;
    Map<uint64_t, ModelFolder> folderTags;

    // The latest latent preview, the prompt that it belongs to, and the size of the image that it stands for.
    const PrDrawImage* PR_NULLABLE preview = nullptr;
    String previewPromptId;
    DimensionSize previewSize;

    // The gallery, what the viewport shows of it, and the record of its selected image, read from the file once.
    Gallery gallery;
    VisitHistory history;
    bool showInfo = false;
    bool showPreview = true;
    ProvenanceRecord selectedRecord;
    String selectedRecordPath;
    uint32_t selectedRecordRevision = 0;
    bool selectedRecordRead = false;
    Array<bool> recordOpen;   // by index of the selected record's operations: true while its row shows what it states

    // The dialog of the server's address, and the dialog of an import.
    bool serverOpen = false;
    ImportPrompt import;

    // The confirmation that a delete asks for, and the images that it names.
    bool deleteOpen = false;
    Array<String> deletePaths;

    PrRandom random = {.state = prRandomSeed()};
};

// Frees what the state holds of the library's objects.
void stateReset(AppState& state);

// Sets the status line, which the status bar shows.
void setStatus(AppState& state, PrStringView text);

// Connects to the state's server, or disconnects from it.
void connectServer(AppState& state);
void disconnectServer(AppState& state);

// Starts a run of the current mode with the current parameters, drawing a seed unless it is locked. Returns false, and
// says why in the status, if the run cannot start.
bool generate(AppState& state);

// Stops the run that the server is running.
void interruptRun(AppState& state);

// Takes the server's events and the landed model surveys into the state, and moves the held events on by `dt` seconds.
// Called once per frame, before the build.
void pumpEvents(AppState& state, double dt);

// Returns the job that the status bar reports: the running job, or else the next pending one, or null.
const Job* PR_NULLABLE activeJob(const AppState& state);

// Returns the number of runs on their way to the server or in flight.
uint32_t runsInFlight(const AppState& state);

// Returns the architecture that the file of `name` in `folder` stated, or Unknown until a survey has read it.
ModelArchitecture modelArchitecture(const AppState& state, ModelFolder folder, PrStringView name);

// Returns true if the adapter `name` and the selected checkpoint state architectures that cannot be used together.
bool loraConflicts(const AppState& state, PrStringView name);

// Chooses the size ladder for the selected checkpoint. Called once per frame, before the build.
void applyDimensions(AppState& state);

// Returns the record that the selected image carries, read from its file on the first request, or null if it carries
// none.
const ProvenanceRecord* PR_NULLABLE selectedRecord(AppState& state);

// Records the selected image as a visit unless the walk stands on it. Called once per frame, before the build.
void noteVisit(AppState& state);

// Walk the visits without recording the walk. Each returns false at its end of the walk.
bool historyBack(AppState& state);
bool historyForward(AppState& state);
bool canGoBack(const AppState& state);
bool canGoForward(const AppState& state);

// The verbs that carry the selected image's record into the parameters. Each takes the newest operation of the record
// that states what it carries, and returns false if none does.
bool usePrompt(AppState& state);
bool useSeed(AppState& state);
bool useAspect(AppState& state);
bool useAll(AppState& state);

// Carries a group of operation `operation` of the selected image's record, counted from the oldest, into the
// parameters. `line` picks the line of a line's group. Returns false if the operation does not state the group.
bool useRecorded(AppState& state, uint32_t operation, UseWhat what, uint32_t line = 0);

// Holds the selected image as the source of the runs, or holds none.
bool useAsSource(AppState& state);
void clearSource(AppState& state);

// Opens the confirmation of deleting the selected images, or deletes the images that it names.
void requestDelete(AppState& state);
void confirmDelete(AppState& state);

// Opens the import's dialog, closes it and forgets what it staged, or stages the image of `pixels`, which are
// straight-alpha sRGB Rgba8. Empty pixels leave what was staged, and say that the clipboard held no image.
void openImport(AppState& state);
void closeImport(AppState& state);
void stageImport(AppState& state, PrPixelView pixels);

// Adds the staged image to the gallery, selects it, and closes the dialog. Returns false, and says why in the dialog,
// if the image could not be written.
bool confirmImport(AppState& state);

// Writes the selected image's pixels, as displayed, to `pixels` as straight-alpha sRGB Rgba8 rows, and returns their
// extent, or zero if no image is selected or its file cannot be read.
PrUVec2 selectedPixels(AppState& state, Array<uint8_t>* pixels);

// Draws a seed below 2^53, so that the seed field shows it exactly.
uint64_t drawSeed(AppState& state);

} // namespace alchemy

PR_ASSUME_NONNULL_END
