// Alchemy's state: the server it talks to, the parameters of the next run, the runs in flight, and the gallery. The
// panels, the commands and the key bindings all read and change it, and the frame loop pumps the server's events into
// it before each build.

#pragma once

#include "pr/base.h"
#include "pr/compress.h"
#include "pr/draw_image.h"
#include "pr/image.h"
#include "pr/string_view.h"
#include "pr/task.h"

#include "examples/alchemy/comfy_client.h"
#include "examples/alchemy/dimensions.h"
#include "examples/alchemy/gallery.h"
#include "examples/alchemy/image_cache.h"
#include "examples/alchemy/models.h"
#include "examples/alchemy/provenance.h"
#include "examples/alchemy/recipes.h"

#include <map>
#include <mutex>
#include <random>
#include <stdint.h>
#include <string>
#include <vector>

PR_ASSUME_NONNULL_BEGIN

namespace alchemy {

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
    std::string name;
    double weight = 1.0;
};

// The inputs that the sampling modes share, and those of Generate. Models are held by the names that the server knows.
// The source image stays held until a command replaces or clears it.
struct GenerateParams {
    std::string prompt;
    std::string negativePrompt;
    std::string sourcePath;
    std::vector<LoraEntry> loras;
    std::string checkpoint;
    std::string sampler;
    std::string scheduler;
    double strength = 0.75;     // how far an image-to-image run may move from its source
    double cfg = 7.0;
    uint64_t seed = 0;
    uint32_t steps = 20;
    uint32_t batch = 1;
    bool seedLocked = false;    // the next run keeps the seed rather than drawing one
};

// The inputs of the other modes.
struct UpscaleParams {
    std::string model;
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
    std::string detector;
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
    std::string promptId;
    JobState state = JobState_Pending;
    uint32_t progressValue = 0;
    uint32_t progressMax = 0;
    std::string node;           // the stage that the run reached, by its node's id
    std::string error;
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
    std::string sourceBytes;
    std::string sourceName;     // the name that the server stored the source under
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
    std::vector<std::string> visits;
    uint32_t position = 0;
};

// The architectures that model files state, by folder and name, read on the task pool.
struct ModelSurvey {
    std::map<std::pair<ModelFolder, std::string>, ModelArchitecture> known;
    std::mutex mutex;
    std::vector<std::pair<std::pair<ModelFolder, std::string>, ModelArchitecture>> landed;
    uint32_t generation = 0;    // the count of catalogs, so that a survey of an older one stops
};

// The state of the application.
struct AppState {
    // The services, which main() owns.
    ComfyClient* client = nullptr;
    ImageCache* images = nullptr;
    PrImageCodec* codec = nullptr;
    PrCompressCodec* compress = nullptr;
    PrTaskPool* pool = nullptr;

    // The server.
    std::string server = "http://127.0.0.1:8188";
    ConnectionState connection = ConnectionState_Disconnected;
    uint32_t queueRemaining = 0;
    std::string status;
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
    std::map<uint64_t, Submission> uploading;
    std::map<uint64_t, Submission> submitting;
    std::vector<Job> jobs;
    std::vector<HeldEvent> held;
    uint64_t catalogTag = 0;
    uint64_t managerTag = 0;
    std::map<uint64_t, ModelFolder> folderTags;

    // The latest latent preview, the prompt that it belongs to, and the size of the image that it stands for.
    const PrDrawImage* PR_NULLABLE preview = nullptr;
    std::string previewPromptId;
    DimensionSize previewSize;

    // The gallery, what the viewport shows of it, and the record of its selected image, read from the file once.
    Gallery gallery;
    VisitHistory history;
    bool showInfo = false;
    bool showPreview = true;
    ProvenanceRecord selectedRecord;
    std::string selectedRecordPath;
    uint32_t selectedRecordRevision = 0;
    bool selectedRecordRead = false;

    // The confirmation that a delete asks for, and the images that it names.
    bool deleteOpen = false;
    std::vector<std::string> deletePaths;

    std::mt19937_64 random{std::random_device{}()};
};

// Frees what the state holds of the library's objects.
void stateReset(AppState& state);

// Sets the status line, which the status bar shows.
void setStatus(AppState& state, std::string text);

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

// Holds the selected image as the source of the runs, or holds none.
bool useAsSource(AppState& state);
void clearSource(AppState& state);

// Opens the confirmation of deleting the selected images, or deletes the images that it names.
void requestDelete(AppState& state);
void confirmDelete(AppState& state);

// Draws a seed below 2^53, so that the seed field shows it exactly.
uint64_t drawSeed(AppState& state);

} // namespace alchemy

PR_ASSUME_NONNULL_END
