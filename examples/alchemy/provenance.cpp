// Provenance: how an image was made, carried in the image.

#include "examples/alchemy/provenance.h"

#include "pr/image_png.h"
#include "pr/json.h"

#include "examples/alchemy/text.h"

#include <stdlib.h>

namespace alchemy {

namespace {

constexpr double kVersion = 1;

void writeText(PrJsonWriter* w, const char* key, const std::string& value) {
    if (value.empty())
        return;
    prJsonWriterKey(w, key);
    prJsonWriterString(w, value);
}

void writeNumber(PrJsonWriter* w, const char* key, double value) {
    if (value == 0.0)
        return;
    prJsonWriterKey(w, key);
    prJsonWriterNumber(w, value);
}

void writeOp(PrJsonWriter* w, const ProvenanceOp& op) {
    prJsonWriterBeginObject(w);
    prJsonWriterKey(w, "mode");
    prJsonWriterString(w, op.mode);
    writeText(w, "prompt", op.prompt);
    writeText(w, "negative", op.negative);
    writeText(w, "checkpoint", op.checkpoint);
    if (!op.loras.empty()) {
        prJsonWriterKey(w, "loras");
        prJsonWriterBeginArray(w);
        for (const ProvenanceLora& lora : op.loras) {
            prJsonWriterBeginObject(w);
            prJsonWriterKey(w, "name");
            prJsonWriterString(w, lora.name);
            prJsonWriterKey(w, "weight");
            prJsonWriterNumber(w, lora.weight);
            prJsonWriterEndObject(w);
        }
        prJsonWriterEndArray(w);
    }
    writeText(w, "sampler", op.sampler);
    writeText(w, "scheduler", op.scheduler);
    writeNumber(w, "steps", op.steps);
    writeNumber(w, "cfg", op.cfg);
    writeNumber(w, "width", op.width);
    writeNumber(w, "height", op.height);
    writeNumber(w, "batch", op.batch);
    writeNumber(w, "strength", op.strength);
    writeText(w, "upscaleModel", op.upscaleModel);
    writeNumber(w, "upscaleFactor", op.factor);
    writeText(w, "direction", op.direction);
    writeNumber(w, "amount", op.amount);
    writeNumber(w, "crossfade", op.crossfade);
    writeNumber(w, "growMask", op.growMask);
    writeNumber(w, "context", op.context);
    writeText(w, "detector", op.detector);
    if (provenanceSamples(op))
        writeText(w, "seed", std::to_string(op.seed));
    writeText(w, "source", op.source);
    prJsonWriterEndObject(w);
}

std::string memberString(const PrJson* d, PrJsonValueIndex object, const char* key) {
    return stringOf(prJsonString(d, prJsonMember(d, object, key)));
}

double memberNumber(const PrJson* d, PrJsonValueIndex object, const char* key, double fallback) {
    PrJsonValueIndex value = prJsonMember(d, object, key);
    return prJsonKind(d, value) == PrJsonKind_Number ? prJsonNumber(d, value, fallback) : fallback;
}

uint32_t memberCount(const PrJson* d, PrJsonValueIndex object, const char* key) {
    double value = memberNumber(d, object, key, 0);
    return value > 0 && value < 4294967296.0 ? uint32_t(value) : 0;
}

ProvenanceOp readOp(const PrJson* d, PrJsonValueIndex object) {
    ProvenanceOp op;
    op.mode = memberString(d, object, "mode");
    op.prompt = memberString(d, object, "prompt");
    op.negative = memberString(d, object, "negative");
    op.checkpoint = memberString(d, object, "checkpoint");
    PrJsonValueIndex loras = prJsonMember(d, object, "loras");
    for (PrJsonValueIndex i = prJsonFirst(d, loras); i != kPrJsonMissing; i = prJsonAfter(d, loras, i)) {
        if (prJsonKind(d, i) == PrJsonKind_Object)
            op.loras.push_back({memberString(d, i, "name"), memberNumber(d, i, "weight", 1.0)});
    }
    op.sampler = memberString(d, object, "sampler");
    op.scheduler = memberString(d, object, "scheduler");
    op.upscaleModel = memberString(d, object, "upscaleModel");
    op.detector = memberString(d, object, "detector");
    op.direction = memberString(d, object, "direction");
    op.source = memberString(d, object, "source");
    op.steps = memberCount(d, object, "steps");
    op.width = memberCount(d, object, "width");
    op.height = memberCount(d, object, "height");
    op.batch = memberCount(d, object, "batch");
    op.amount = memberCount(d, object, "amount");
    op.crossfade = memberCount(d, object, "crossfade");
    op.growMask = memberCount(d, object, "growMask");
    op.context = memberCount(d, object, "context");
    op.cfg = memberNumber(d, object, "cfg", 0);
    op.strength = memberNumber(d, object, "strength", 0);
    op.factor = memberNumber(d, object, "upscaleFactor", 0);
    std::string seed = memberString(d, object, "seed");
    op.seed = seed.empty() ? 0 : strtoull(seed.c_str(), nullptr, 10);
    return op;
}

// Keeps the text of a PNG file's chunk under the provenance keyword.
void collect(void* PR_NULLABLE context, const PrImagePngText* text) {
    if (text->keyword == kProvenanceKeyword)
        *static_cast<std::string*>(context) = stringOf(text->text);
}

} // namespace

std::string provenanceSerialize(const ProvenanceRecord& record) {
    PrJsonWriter* w = prJsonWriterCreate();
    prJsonWriterBeginObject(w);
    prJsonWriterKey(w, "version");
    prJsonWriterNumber(w, kVersion);
    prJsonWriterKey(w, "operations");
    prJsonWriterBeginArray(w);
    for (const ProvenanceOp& op : record.operations)
        writeOp(w, op);
    prJsonWriterEndArray(w);
    prJsonWriterEndObject(w);
    std::string text = stringOf(prJsonWriterText(w));
    prJsonWriterDestroy(w);
    return text;
}

bool provenanceParse(PrStringView text, ProvenanceRecord* out) {
    PrJson* d = prJsonCreate();
    bool parsed = prJsonParse(d, text, nullptr);
    PrJsonValueIndex operations = parsed ? prJsonMember(d, 0, "operations") : kPrJsonMissing;
    bool ok = prJsonKind(d, operations) == PrJsonKind_Array;
    if (ok) {
        out->operations.clear();
        for (PrJsonValueIndex i = prJsonFirst(d, operations); i != kPrJsonMissing; i = prJsonAfter(d, operations, i)) {
            if (prJsonKind(d, i) == PrJsonKind_Object)
                out->operations.push_back(readOp(d, i));
        }
    }
    prJsonDestroy(d);
    return ok;
}

bool provenanceRead(PrByteView png, PrCompressCodec* PR_NULLABLE compress, ProvenanceRecord* out) {
    std::string text;
    PrStatus status = prImagePngEachText(png, compress, collect, &text);
    if (status != PrStatus_Ok && status != PrStatus_Truncated)
        return false;
    return !text.empty() && provenanceParse(text, out);
}

std::string provenanceWrite(PrByteView png, const ProvenanceRecord& record) {
    std::string text = provenanceSerialize(record);
    size_t size = 0;
    if (prImagePngSetText(png, kProvenanceKeyword, text, nullptr, 0, &size) != PrStatus_Ok)
        return std::string();
    std::string out(size, '\0');
    prImagePngSetText(png, kProvenanceKeyword, text, reinterpret_cast<uint8_t*>(out.data()), size, &size);
    return out;
}

const ProvenanceOp* PR_NULLABLE provenanceNewest(const ProvenanceRecord& record) {
    return record.operations.empty() ? nullptr : &record.operations.back();
}

bool provenanceSamples(const ProvenanceOp& op) {
    return !op.checkpoint.empty() || !op.sampler.empty();
}

std::string provenanceChain(const ProvenanceRecord& record) {
    std::string out;
    for (const ProvenanceOp& op : record.operations) {
        if (!out.empty())
            out += " \xE2\x86\x92 ";
        out += op.mode.empty() ? std::string("?") : op.mode;
    }
    return out;
}

} // namespace alchemy
