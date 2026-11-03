// Alchemy's state, and the runs that it sends to the server.

#include "examples/alchemy/state.h"

#include "pr/ascii.h"
#include "pr/file.h"

#include "examples/alchemy/prompt_text.h"
#include "examples/alchemy/text.h"

#include <algorithm>
#include <cmath>

namespace alchemy {

using namespace pr;

namespace {

// The seconds that an event for an unknown prompt is held before it is dropped.
constexpr double kHeldEventMaxAge = 5.0;

// The longest the visit history grows.
constexpr size_t kHistoryLimit = 100;

std::string readFile(const std::string& path) {
    PrFileMapping mapping{};
    if (!prFileMap(path, &mapping))
        return std::string();
    std::string bytes(reinterpret_cast<const char*>(mapping.data), mapping.size);
    prFileUnmap(&mapping);
    return bytes;
}

PrByteView bytesOf(const std::string& bytes) {
    return PrByteView{reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size()};
}

// Returns the name in `names` that equals `current`, or else the first that contains `preferred`, or else the first.
std::string chooseName(const std::vector<std::string>& names, const std::string& current, PrStringView preferred) {
    if (std::find(names.begin(), names.end(), current) != names.end())
        return current;
    for (const std::string& name : names) {
        if (preferred.size != 0 && prAsciiContainsFold(name, preferred))
            return name;
    }
    return names.empty() ? std::string() : names.front();
}

// The size of the image that a submission produces, or zero if only the finished image says.
DimensionSize outputSize(const Submission& s) {
    DimensionSize source = s.sourceSize;
    if (s.mode != OperationMode_Generate && (source.width == 0 || source.height == 0))
        return {};
    switch (s.mode) {
        case OperationMode_Generate: return s.size;
        case OperationMode_Upscale:
            return {
                uint32_t(std::lround(source.width * s.upscale.factor)),
                uint32_t(std::lround(source.height * s.upscale.factor)),
            };
        case OperationMode_Outpaint: {
            bool horizontal =
                s.outpaint.direction == OutpaintDirection_Left || s.outpaint.direction == OutpaintDirection_Right;
            return {
                source.width + (horizontal ? s.outpaint.amount : 0),
                source.height + (horizontal ? 0 : s.outpaint.amount),
            };
        }
        default: return source;
    }
}

// Returns the operation that a submission records: the fields that its mode uses and nothing it leaves at a default.
ProvenanceOp operationOf(const Submission& s) {
    ProvenanceOp op;
    op.mode = stringOf(modeName(s.mode));
    op.source = stringOf(pathFilename(s.sampling.sourcePath));
    if (modeSamples(s.mode)) {
        for (const LoraEntry& lora : s.sampling.loras)
            op.loras.push_back({lora.name, lora.weight});
        op.prompt = s.sampling.prompt;
        op.negative = s.sampling.negativePrompt;
        op.checkpoint = s.sampling.checkpoint;
        op.sampler = s.sampling.sampler;
        op.scheduler = s.sampling.scheduler;
        op.steps = s.sampling.steps;
        op.cfg = s.sampling.cfg;
        op.seed = s.sampling.seed;
    }
    switch (s.mode) {
        case OperationMode_Generate:
            op.width = s.size.width;
            op.height = s.size.height;
            op.batch = s.sampling.batch;
            if (!s.sampling.sourcePath.empty())
                op.strength = s.sampling.strength;
            break;
        case OperationMode_Upscale:
            op.upscaleModel = s.upscale.model;
            op.factor = s.upscale.factor;
            break;
        case OperationMode_Outpaint:
            op.direction = stringOf(directionName(s.outpaint.direction));
            op.amount = s.outpaint.amount;
            op.crossfade = s.outpaint.crossfade;
            op.growMask = s.outpaint.growMask;
            op.context = s.outpaint.context;
            break;
        case OperationMode_Detail: op.detector = s.detail.detector; break;
    }
    return op;
}

SamplingRecipe samplingOf(const GenerateParams& params) {
    SamplingRecipe recipe{
        .checkpoint = params.checkpoint,
        .sampler = params.sampler,
        .scheduler = params.scheduler,
        .prompt = params.prompt,
        .negativePrompt = params.negativePrompt,
        .seed = params.seed,
        .steps = params.steps,
        .cfg = params.cfg,
    };
    for (const LoraEntry& lora : params.loras)
        recipe.loras.push_back({lora.name, lora.weight});
    return recipe;
}

std::string graphOf(const Submission& s) {
    switch (s.mode) {
        case OperationMode_Upscale:
            return upscaleGraph({.model = s.upscale.model, .sourceImage = s.sourceName, .factor = s.upscale.factor});
        case OperationMode_Outpaint:
            return outpaintGraph({
                .sampling = samplingOf(s.sampling),
                .sourceImage = s.sourceName,
                .direction = s.outpaint.direction,
                .sourceWidth = s.sourceSize.width,
                .sourceHeight = s.sourceSize.height,
                .amount = s.outpaint.amount,
                .crossfade = s.outpaint.crossfade,
                .growMask = s.outpaint.growMask,
                .context = s.outpaint.context,
            });
        case OperationMode_Detail:
            return detailGraph(
                {.sampling = samplingOf(s.sampling), .sourceImage = s.sourceName, .detector = s.detail.detector});
        default:
            return txt2ImgGraph({
                .sampling = samplingOf(s.sampling),
                .sourceImage = s.sourceName,
                .denoise = s.sampling.strength,
                .width = s.size.width,
                .height = s.size.height,
                .batch = s.sampling.batch,
            });
    }
}

// Sends a submission whose source, if it has one, the server holds.
void submit(AppState& state, Submission submission) {
    uint64_t tag = state.nextTag++;
    comfySubmit(state.client, tag, graphOf(submission));
    state.submitting[tag] = std::move(submission);
}

// Fills what every sampling mode reads, drawing a seed unless it is locked, so that the panel shows the seed that made
// the image in front of the user.
bool fillSampling(AppState& state, Submission& s) {
    if (state.params.checkpoint.empty()) {
        setStatus(state, "The server has no checkpoint to sample with.");
        return false;
    }
    if (!state.params.seedLocked)
        state.params.seed = drawSeed(state);
    s.sampling = state.params;
    return true;
}

bool fillSource(AppState& state, Submission& s) {
    if (state.params.sourcePath.empty()) {
        setStatus(state, "Choose a source image first: select an image and use it as the source.");
        return false;
    }
    s.sampling.sourcePath = state.params.sourcePath;
    return true;
}

Job* findJob(AppState& state, const std::string& promptId) {
    for (Job& job : state.jobs) {
        if (job.promptId == promptId)
            return &job;
    }
    return nullptr;
}

void dropPreview(AppState& state) {
    prDrawImageRelease(state.preview);
    state.preview = nullptr;
    state.previewPromptId.clear();
}

void finishJob(Job& job, AppState& state, JobState outcome, std::string error) {
    job.state = outcome;
    job.error = std::move(error);
    if (state.previewPromptId == job.promptId)
        dropPreview(state);
    setStatus(state, outcome == JobState_Done ? "The run finished." : "The run failed: " + job.error);
}

// Writes a finished image into the gallery with the record of the run that made it.
void saveImage(AppState& state, const ComfyEvent& event, const Job* PR_NULLABLE job) {
    std::string png(event.bytes.begin(), event.bytes.end());
    if (job != nullptr) {
        std::string recorded = provenanceWrite(bytesOf(png), job->record);
        // An image that cannot carry its record is still worth keeping.
        if (!recorded.empty())
            png = std::move(recorded);
    }
    std::string saved;
    if (gallerySave(state.gallery, event.filename, png, &saved))
        setStatus(state, "Saved " + stringOf(pathFilename(saved)) + ".");
    else
        setStatus(state, "Could not save " + event.filename + " in " + state.gallery.dir + ".");
}

// Takes the reply to a catalog request into the state, and starts the survey of its models' files.
void adoptCatalog(AppState& state, const ComfyCatalog& catalog) {
    state.catalog = catalog;
    GenerateParams& p = state.params;
    p.checkpoint = chooseName(catalog.checkpoints, p.checkpoint, "xl");
    p.sampler = chooseName(catalog.samplers, p.sampler, "euler");
    p.scheduler = chooseName(catalog.schedulers, p.scheduler, "normal");
    state.upscale.model = chooseName(catalog.upscaleModels, state.upscale.model, "x4");
    state.detail.detector = chooseName(catalog.detectors, state.detail.detector, "face");
    // An adapter that the server no longer has is dropped from the chain.
    std::vector<LoraEntry>& loras = p.loras;
    loras.erase(std::remove_if(loras.begin(), loras.end(), [&](const LoraEntry& lora) {
        return std::find(catalog.loras.begin(), catalog.loras.end(), lora.name) == catalog.loras.end();
    }), loras.end());
    {
        std::lock_guard<std::mutex> lock(state.survey.mutex);
        ++state.survey.generation;
    }
    for (ModelFolder folder : {ModelFolder_Checkpoints, ModelFolder_Loras}) {
        uint64_t tag = state.nextTag++;
        state.folderTags[tag] = folder;
        comfyFetchFolderPaths(state.client, tag, stringOf(modelFolderName(folder)));
    }
    setStatus(state, "Connected to " + state.server + ".");
}

// A survey of one folder's models, which a pool thread reads.
struct Survey {
    ModelSurvey* survey;
    uint32_t generation;
    ModelFolder folder;
    std::vector<std::string> roots;
    std::vector<std::string> names;
};

void runSurvey(void* PR_NULLABLE context) {
    Survey* job = static_cast<Survey*>(context);
    for (const std::string& name : job->names) {
        {
            std::lock_guard<std::mutex> lock(job->survey->mutex);
            if (job->survey->generation != job->generation)
                break;
        }
        ModelArchitecture arch = ModelArchitecture_Unknown;
        for (const std::string& root : job->roots) {
            arch = readArchitecture(root + "/" + name);
            if (arch != ModelArchitecture_Unknown)
                break;
        }
        std::lock_guard<std::mutex> lock(job->survey->mutex);
        job->survey->landed.push_back({{job->folder, name}, arch});
    }
    delete job;
}

// Starts reading the files of a folder's models through the directories that the server searches. A server on another
// machine names directories that this process cannot open, which leaves its models unknown.
void startSurvey(AppState& state, ModelFolder folder, std::vector<std::string> roots) {
    const std::vector<std::string>& names =
        folder == ModelFolder_Checkpoints ? state.catalog.checkpoints : state.catalog.loras;
    if (roots.empty() || names.empty())
        return;
    uint32_t generation = 0;
    {
        std::lock_guard<std::mutex> lock(state.survey.mutex);
        generation = state.survey.generation;
    }
    prTaskPoolPost(state.pool, runSurvey, new Survey{&state.survey, generation, folder, std::move(roots), names});
}

// Applies an event to the state. Returns false if it names a prompt that no job has yet, which the caller holds.
bool applyEvent(AppState& state, const ComfyEvent& event) {
    switch (event.kind) {
        case ComfyEventKind_Connected: {
            state.connection = ConnectionState_Connected;
            setStatus(state, "Connected to " + state.server + ". Reading its models...");
            state.catalogTag = state.nextTag++;
            comfyFetchCatalog(state.client, state.catalogTag);
            state.managerTag = state.nextTag++;
            comfyFetchManagerVersion(state.client, state.managerTag);
            return true;
        }
        case ComfyEventKind_Disconnected:
            state.connection = ConnectionState_Disconnected;
            state.queueRemaining = 0;
            state.jobs.clear();
            state.held.clear();
            state.uploading.clear();
            state.submitting.clear();
            dropPreview(state);
            setStatus(state, event.message);
            return true;
        case ComfyEventKind_QueueStatus: state.queueRemaining = event.count; return true;
        case ComfyEventKind_Progress:
        case ComfyEventKind_Executing: {
            Job* job = findJob(state, event.promptId);
            if (job == nullptr)
                return false;
            job->state = JobState_Running;
            job->node = event.node;
            if (event.kind == ComfyEventKind_Progress) {
                job->progressValue = event.value;
                job->progressMax = event.max;
            }
            return true;
        }
        case ComfyEventKind_Preview: return true;
        case ComfyEventKind_ImageReady: {
            Job* job = findJob(state, event.promptId);
            if (job == nullptr && !event.promptId.empty())
                return false;
            saveImage(state, event, job);
            return true;
        }
        case ComfyEventKind_ImageFailed: setStatus(state, "Could not fetch " + event.filename + "."); return true;
        case ComfyEventKind_Completed:
        case ComfyEventKind_Failed: {
            Job* job = findJob(state, event.promptId);
            if (job == nullptr)
                return false;
            finishJob(*job, state, event.kind == ComfyEventKind_Completed ? JobState_Done : JobState_Failed,
                event.message);
            return true;
        }
        default: break;
    }

    // Replies to the requests that the state made, by their tags.
    bool replied = event.kind == ComfyEventKind_Replied;
    if (event.tag == state.catalogTag) {
        if (replied && event.catalog != nullptr)
            adoptCatalog(state, *event.catalog);
        else
            setStatus(state, event.message);
    } else if (event.tag == state.managerTag) {
        state.managerPresent = replied;
    } else if (state.folderTags.count(event.tag) != 0) {
        ModelFolder folder = state.folderTags[event.tag];
        state.folderTags.erase(event.tag);
        if (replied)
            startSurvey(state, folder, event.names);
    } else if (state.uploading.count(event.tag) != 0) {
        Submission submission = std::move(state.uploading[event.tag]);
        state.uploading.erase(event.tag);
        if (!replied) {
            setStatus(state, event.message);
            return true;
        }
        submission.sourceName = event.name;
        submit(state, std::move(submission));
    } else if (state.submitting.count(event.tag) != 0) {
        Submission submission = std::move(state.submitting[event.tag]);
        state.submitting.erase(event.tag);
        if (!replied) {
            setStatus(state, event.message);
            return true;
        }
        Job job;
        job.promptId = event.promptId;
        job.output = outputSize(submission);
        // The run continues the chain that its source carries.
        if (!submission.sourceBytes.empty())
            provenanceRead(bytesOf(submission.sourceBytes), state.compress, &job.record);
        job.record.operations.push_back(operationOf(submission));
        state.jobs.push_back(std::move(job));
        setStatus(state, "The server queued the run.");
    }
    return true;
}

void adoptSurveys(AppState& state) {
    std::lock_guard<std::mutex> lock(state.survey.mutex);
    for (std::pair<std::pair<ModelFolder, std::string>, ModelArchitecture>& found : state.survey.landed)
        state.survey.known[found.first] = found.second;
    state.survey.landed.clear();
}

bool canSampleFrom(const ProvenanceOp& op) {
    return provenanceSamples(op);
}

// Returns the newest operation of the selected image's record for which `wanted` holds, or null.
template <typename Predicate>
const ProvenanceOp* PR_NULLABLE newestWhere(AppState& state, Predicate wanted) {
    const ProvenanceRecord* record = selectedRecord(state);
    if (record == nullptr)
        return nullptr;
    for (size_t i = record->operations.size(); i > 0; --i) {
        if (wanted(record->operations[i - 1]))
            return &record->operations[i - 1];
    }
    return nullptr;
}

void useLoras(AppState& state, const ProvenanceOp& op) {
    GenerateParams& p = state.params;
    p.loras.clear();
    for (const ProvenanceLora& lora : op.loras) {
        if (std::find(state.catalog.loras.begin(), state.catalog.loras.end(), lora.name) != state.catalog.loras.end())
            p.loras.push_back({lora.name, lora.weight});
    }
}

void useSampling(AppState& state, const ProvenanceOp& op) {
    GenerateParams& p = state.params;
    p.sampler = chooseName(state.catalog.samplers, op.sampler, "");
    p.scheduler = chooseName(state.catalog.schedulers, op.scheduler, "");
    if (op.steps != 0)
        p.steps = op.steps;
    if (op.cfg != 0)
        p.cfg = op.cfg;
}

void useAllOf(AppState& state, const ProvenanceOp& op) {
    GenerateParams& p = state.params;
    p.prompt = op.prompt;
    p.negativePrompt = op.negative;
    p.checkpoint = chooseName(state.catalog.checkpoints, op.checkpoint, "");
    useLoras(state, op);
    useSampling(state, op);
    p.seed = op.seed;
    p.seedLocked = true;
    if (op.width != 0 && op.height != 0 && !adoptExactSize(state.dims, {op.width, op.height})) {
        setCustomSize(state.dims, true);
        state.dims.size = {op.width, op.height};
    }
}

// Returns the line `index` of `text`, or the empty view.
PrStringView lineOf(PrStringView text, uint32_t index) {
    std::vector<PrStringView> lines = promptLines(text);
    return index < lines.size() ? lines[index] : PrStringView("");
}

} // namespace

PrStringView modeName(OperationMode mode) {
    switch (mode) {
        case OperationMode_Upscale: return "upscale";
        case OperationMode_Outpaint: return "outpaint";
        case OperationMode_Detail: return "detail";
        default: return "generate";
    }
}

OperationMode parseMode(PrStringView name, OperationMode fallback) {
    for (OperationMode mode = 0; mode < OperationMode_Count; ++mode) {
        if (prAsciiContainsFold(name, modeName(mode)))
            return mode;
    }
    return fallback;
}

bool modeNeedsSource(OperationMode mode) {
    return mode != OperationMode_Generate;
}

bool modeSamples(OperationMode mode) {
    return mode != OperationMode_Upscale;
}

UseWhat parseUseWhat(PrStringView name) {
    static const char* const kNames[UseWhat_Count] = {
        "prompt", "negative", "promptLine", "negativeLine", "model", "loras", "sampling", "aspect", "seed", "strength",
        "upscale", "outpaint", "detector", "source", "all",
    };
    for (UseWhat what = 0; what < UseWhat_Count; ++what) {
        if (name == kNames[what])
            return what;
    }
    return UseWhat_Count;
}

void stateReset(AppState& state) {
    closeImport(state);
    dropPreview(state);
    // A survey still on the pool stops at its next name.
    std::lock_guard<std::mutex> lock(state.survey.mutex);
    ++state.survey.generation;
}

void setStatus(AppState& state, std::string text) {
    state.status = std::move(text);
}

void connectServer(AppState& state) {
    state.jobs.clear();
    state.held.clear();
    state.uploading.clear();
    state.submitting.clear();
    dropPreview(state);
    state.connection = ConnectionState_Connecting;
    setStatus(state, "Connecting to " + state.server + "...");
    state.connectedServer = state.server;
    comfyConnect(state.client, state.server);
}

void disconnectServer(AppState& state) {
    comfyDisconnect(state.client);
}

bool generate(AppState& state) {
    if (state.connection != ConnectionState_Connected) {
        setStatus(state, "Not connected to a server.");
        return false;
    }
    Submission s;
    s.mode = state.mode;
    s.size = state.dims.size;
    switch (state.mode) {
        case OperationMode_Generate:
            if (!fillSampling(state, s))
                return false;
            break;
        case OperationMode_Upscale:
            if (state.upscale.model.empty()) {
                setStatus(state, "The server has no upscale model.");
                return false;
            }
            if (!fillSource(state, s))
                return false;
            s.upscale = state.upscale;
            break;
        case OperationMode_Outpaint:
            if (!fillSampling(state, s) || !fillSource(state, s))
                return false;
            s.outpaint = state.outpaint;
            break;
        case OperationMode_Detail:
            if (state.detail.detector.empty()) {
                setStatus(state, "The server does not have the detailer nodes installed.");
                return false;
            }
            if (!fillSampling(state, s) || !fillSource(state, s))
                return false;
            s.detail = state.detail;
            break;
    }
    if (s.sampling.sourcePath.empty()) {
        submit(state, std::move(s));
        setStatus(state, "Submitted the run.");
        return true;
    }
    s.sourceBytes = readFile(s.sampling.sourcePath);
    const PrDrawImage* source = imageDecode(state.codec, s.sourceBytes.data(), s.sourceBytes.size());
    if (source == nullptr) {
        setStatus(state, "Could not read " + stringOf(pathFilename(s.sampling.sourcePath)) + ".");
        return false;
    }
    PrUVec2 extent = prDrawImageExtent(source);
    prDrawImageRelease(source);
    s.sourceSize = {extent.x, extent.y};
    uint64_t tag = state.nextTag++;
    std::vector<uint8_t> bytes(s.sourceBytes.begin(), s.sourceBytes.end());
    comfyUpload(state.client, tag, stringOf(pathFilename(s.sampling.sourcePath)), std::move(bytes));
    state.uploading[tag] = std::move(s);
    setStatus(state, "Uploading the source image...");
    return true;
}

void interruptRun(AppState& state) {
    comfyInterrupt(state.client);
    setStatus(state, "Asked the server to stop the run.");
}

void pumpEvents(AppState& state, double dt) {
    adoptSurveys(state);
    const PrDrawImage* preview = nullptr;
    std::string previewPrompt;
    ComfyEvent event;
    while (comfyPoll(state.client, &event)) {
        // Only the newest frame of a preview is decoded.
        if (event.kind == ComfyEventKind_Preview) {
            prDrawImageRelease(preview);
            preview = imageDecode(state.codec, event.bytes.data(), event.bytes.size());
            const Job* job = activeJob(state);
            previewPrompt = job != nullptr ? job->promptId : std::string();
            continue;
        }
        if (!applyEvent(state, event))
            state.held.push_back({std::move(event), 0});
    }
    if (preview != nullptr && !previewPrompt.empty()) {
        dropPreview(state);
        state.preview = preview;
        state.previewPromptId = previewPrompt;
        const Job* job = findJob(state, previewPrompt);
        state.previewSize = job != nullptr ? job->output : DimensionSize{};
    } else {
        prDrawImageRelease(preview);
    }
    for (size_t i = 0; i < state.held.size();) {
        if (applyEvent(state, state.held[i].event)) {
            state.held.erase(state.held.begin() + long(i));
            continue;
        }
        state.held[i].age += dt;
        if (state.held[i].age >= kHeldEventMaxAge) {
            state.held.erase(state.held.begin() + long(i));
            continue;
        }
        ++i;
    }
    // The status bar shows what is in flight. A finished run left its image in the gallery and its outcome in the
    // status.
    state.jobs.erase(std::remove_if(state.jobs.begin(), state.jobs.end(), [](const Job& job) {
        return job.state == JobState_Done || job.state == JobState_Failed;
    }), state.jobs.end());
}

const Job* PR_NULLABLE activeJob(const AppState& state) {
    for (const Job& job : state.jobs) {
        if (job.state == JobState_Running)
            return &job;
    }
    for (const Job& job : state.jobs) {
        if (job.state == JobState_Pending)
            return &job;
    }
    return nullptr;
}

uint32_t runsInFlight(const AppState& state) {
    return uint32_t(state.jobs.size() + state.uploading.size() + state.submitting.size());
}

ModelArchitecture modelArchitecture(const AppState& state, ModelFolder folder, PrStringView name) {
    std::map<std::pair<ModelFolder, std::string>, ModelArchitecture>::const_iterator found =
        state.survey.known.find({folder, stringOf(name)});
    return found != state.survey.known.end() ? found->second : ModelArchitecture_Unknown;
}

bool loraConflicts(const AppState& state, PrStringView name) {
    return architecturesConflict(modelArchitecture(state, ModelFolder_Loras, name),
        modelArchitecture(state, ModelFolder_Checkpoints, state.params.checkpoint));
}

void applyDimensions(AppState& state) {
    seedDimensions(state.dims, state.params.checkpoint,
        modelArchitecture(state, ModelFolder_Checkpoints, state.params.checkpoint));
}

const ProvenanceRecord* PR_NULLABLE selectedRecord(AppState& state) {
    const GalleryEntry* entry = gallerySelected(state.gallery);
    if (entry == nullptr)
        return nullptr;
    if (!state.selectedRecordRead || state.selectedRecordPath != entry->path ||
            state.selectedRecordRevision != state.gallery.revision) {
        state.selectedRecordRead = true;
        state.selectedRecordPath = entry->path;
        state.selectedRecordRevision = state.gallery.revision;
        state.selectedRecord = ProvenanceRecord{};
        std::string bytes = readFile(entry->path);
        provenanceRead(bytesOf(bytes), state.compress, &state.selectedRecord);
        // The run that produced the image opens first, since it is what the record is asked about.
        state.recordOpen.assign(state.selectedRecord.operations.size(), false);
        if (!state.recordOpen.empty())
            state.recordOpen.back() = true;
    }
    return state.selectedRecord.operations.empty() ? nullptr : &state.selectedRecord;
}

void noteVisit(AppState& state) {
    const GalleryEntry* entry = gallerySelected(state.gallery);
    if (entry == nullptr)
        return;
    VisitHistory& h = state.history;
    if (h.position < h.visits.size() && h.visits[h.position] == entry->path)
        return;
    // A visit made after stepping back discards the walk ahead of it.
    if (!h.visits.empty())
        h.visits.resize(h.position + 1);
    h.visits.push_back(entry->path);
    if (h.visits.size() > kHistoryLimit)
        h.visits.erase(h.visits.begin());
    h.position = uint32_t(h.visits.size() - 1);
}

bool historyBack(AppState& state) {
    VisitHistory& h = state.history;
    for (uint32_t at = h.position; at > 0; --at) {
        uint32_t index = galleryIndexOf(state.gallery, h.visits[at - 1]);
        if (index < state.gallery.entries.size()) {
            h.position = at - 1;
            gallerySelect(state.gallery, index);
            return true;
        }
    }
    return false;
}

bool historyForward(AppState& state) {
    VisitHistory& h = state.history;
    for (uint32_t at = h.position + 1; at < h.visits.size(); ++at) {
        uint32_t index = galleryIndexOf(state.gallery, h.visits[at]);
        if (index < state.gallery.entries.size()) {
            h.position = at;
            gallerySelect(state.gallery, index);
            return true;
        }
    }
    return false;
}

bool canGoBack(const AppState& state) {
    return state.history.position > 0;
}

bool canGoForward(const AppState& state) {
    return state.history.position + 1 < state.history.visits.size();
}

bool usePrompt(AppState& state) {
    const ProvenanceOp* op = newestWhere(state, [](const ProvenanceOp& o) { return !o.prompt.empty(); });
    if (op == nullptr)
        return false;
    state.params.prompt = op->prompt;
    state.params.negativePrompt = op->negative;
    return true;
}

bool useSeed(AppState& state) {
    const ProvenanceOp* op = newestWhere(state, canSampleFrom);
    if (op == nullptr)
        return false;
    state.params.seed = op->seed;
    state.params.seedLocked = true;
    return true;
}

bool useAspect(AppState& state) {
    const ProvenanceOp* op = newestWhere(state, [](const ProvenanceOp& o) { return o.width != 0 && o.height != 0; });
    if (op == nullptr)
        return false;
    applyAspectRatio(state.dims, {op->width, op->height});
    return true;
}

bool useAll(AppState& state) {
    const ProvenanceOp* op = newestWhere(state, canSampleFrom);
    if (op == nullptr)
        return false;
    useAllOf(state, *op);
    return true;
}

bool useRecorded(AppState& state, uint32_t operation, UseWhat what, uint32_t line) {
    const ProvenanceRecord* record = selectedRecord(state);
    if (record == nullptr || operation >= record->operations.size())
        return false;
    const ProvenanceOp& op = record->operations[operation];
    GenerateParams& p = state.params;
    switch (what) {
        case UseWhat_Prompt:
            if (op.prompt.empty())
                return false;
            p.prompt = op.prompt;
            return true;
        case UseWhat_Negative:
            if (op.negative.empty())
                return false;
            p.negativePrompt = op.negative;
            return true;
        case UseWhat_PromptLine:
        case UseWhat_NegativeLine: {
            bool positive = what == UseWhat_PromptLine;
            PrStringView text = lineOf(positive ? op.prompt : op.negative, line);
            if (text.size == 0)
                return false;
            std::string& into = positive ? p.prompt : p.negativePrompt;
            into = promptAppendLine(into, text);
            return true;
        }
        case UseWhat_Model:
            if (op.checkpoint.empty())
                return false;
            p.checkpoint = chooseName(state.catalog.checkpoints, op.checkpoint, "");
            return true;
        case UseWhat_Loras:
            if (op.loras.empty())
                return false;
            useLoras(state, op);
            return true;
        case UseWhat_Sampling:
            if (!provenanceSamples(op))
                return false;
            useSampling(state, op);
            return true;
        case UseWhat_Aspect:
            if (op.width == 0 || op.height == 0)
                return false;
            applyAspectRatio(state.dims, {op.width, op.height});
            return true;
        case UseWhat_Seed:
            if (!provenanceSamples(op))
                return false;
            p.seed = op.seed;
            p.seedLocked = true;
            return true;
        case UseWhat_Strength:
            if (op.strength == 0)
                return false;
            p.strength = op.strength;
            return true;
        case UseWhat_Upscale:
            if (op.upscaleModel.empty())
                return false;
            state.upscale.model = chooseName(state.catalog.upscaleModels, op.upscaleModel, "");
            if (op.factor != 0)
                state.upscale.factor = op.factor;
            return true;
        case UseWhat_Outpaint:
            if (op.direction.empty())
                return false;
            state.outpaint.direction = parseDirection(op.direction, state.outpaint.direction);
            state.outpaint.amount = op.amount;
            state.outpaint.crossfade = op.crossfade;
            state.outpaint.growMask = op.growMask;
            state.outpaint.context = op.context;
            return true;
        case UseWhat_Detector:
            if (op.detector.empty())
                return false;
            state.detail.detector = op.detector;
            return true;
        case UseWhat_Source:
            if (op.source.empty())
                return false;
            p.sourcePath = op.source;
            return true;
        case UseWhat_All:
            if (!provenanceSamples(op))
                return false;
            useAllOf(state, op);
            return true;
        default:
            return false;
    }
}

bool useAsSource(AppState& state) {
    const GalleryEntry* entry = gallerySelected(state.gallery);
    if (entry == nullptr)
        return false;
    state.params.sourcePath = entry->path;
    setStatus(state, "The source image is " + entry->filename + ".");
    return true;
}

void clearSource(AppState& state) {
    state.params.sourcePath.clear();
}

void requestDelete(AppState& state) {
    state.deletePaths = gallerySelectionPaths(state.gallery);
    state.deleteOpen = !state.deletePaths.empty();
}

void confirmDelete(AppState& state) {
    for (const std::string& path : state.deletePaths)
        imageCacheForget(state.images, path);
    uint32_t removed = galleryDelete(state.gallery, state.deletePaths);
    setStatus(state, "Deleted " + std::to_string(removed) + (removed == 1 ? " image." : " images."));
    state.deleteOpen = false;
    state.deletePaths.clear();
}

void openImport(AppState& state) {
    state.import.open = true;
}

void closeImport(AppState& state) {
    prDrawImageRelease(state.import.image);
    state.import = ImportPrompt{};
}

void stageImport(AppState& state, PrPixelView pixels) {
    state.import.open = true;
    if (pixels.extent.x == 0 || pixels.extent.y == 0) {
        state.import.message = "The clipboard holds no image.";
        return;
    }
    PrDrawImageDesc desc{
        .pixels = pixels,
        .format = PrPixelFormat_Rgba8,
        .alpha = PrDrawAlpha_Straight,
        .space = PrColorSpace_Srgb,
    };
    const PrDrawImage* image = nullptr;
    if (prDrawImageCreate(&desc, &image) != PrStatus_Ok) {
        state.import.message = "The clipboard's image is too large.";
        return;
    }
    prDrawImageRelease(state.import.image);
    state.import.image = image;
    state.import.message.clear();
}

bool confirmImport(AppState& state) {
    if (state.import.image == nullptr)
        return false;
    PrPixelView pixels = prDrawImagePixels(state.import.image);
    size_t size = 0;
    prImageCodecEncode(state.codec, "image/png", pixels, nullptr, 0, &size);
    std::string png(size, '\0');
    std::string saved;
    bool encoded = prImageCodecEncode(state.codec, "image/png", pixels, reinterpret_cast<uint8_t*>(png.data()), size,
        &size) == PrStatus_Ok;
    if (!encoded || !gallerySave(state.gallery, "Imported.png", png, &saved)) {
        state.import.message = "The image could not be written to the gallery.";
        return false;
    }
    setStatus(state, "Imported " + stringOf(pathFilename(saved)) + ".");
    closeImport(state);
    return true;
}

PrUVec2 selectedPixels(AppState& state, std::vector<uint8_t>* pixels) {
    const GalleryEntry* entry = gallerySelected(state.gallery);
    if (entry == nullptr)
        return PrUVec2{0, 0};
    std::string bytes = readFile(entry->path);
    PrImageSource source{};
    source.memory = bytesOf(bytes);
    PrImageDecoder* decoder = prImageDecoderCreate(state.codec);
    PrUVec2 extent{0, 0};
    if (prImageDecoderOpen(decoder, source, nullptr) == PrStatus_Ok) {
        const PrImageInfo* info = prImageDecoderInfo(decoder);
        bool swaps = prImageOrientationSwapsAxes(info->orientation);
        PrUVec2 shown = swaps ? PrUVec2{info->extent.y, info->extent.x} : info->extent;
        pixels->resize(size_t(shown.x) * shown.y * 4);
        PrPixels into{pixels->data(), size_t(shown.x) * 4, shown};
        if (prImageDecoderReadRgba8(decoder, 0, PrImageAlpha_Straight, info->orientation, into) == PrStatus_Ok)
            extent = shown;
    }
    prImageDecoderDestroy(decoder);
    return extent;
}

uint64_t drawSeed(AppState& state) {
    return state.random() & ((uint64_t(1) << 53) - 1);
}

} // namespace alchemy
