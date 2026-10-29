// Provenance: how an image was made, carried in the image.
//
// The record is a chain. Every run appends its own operation to whatever the image it worked from already carried, so a
// generate, an outpaint and an upscale read back as three operations, in the order that they ran. The record is JSON in
// a text chunk of the PNG (pr/image_png.h), under kProvenanceKeyword.
//
// A record that a newer build wrote may name a mode that this one does not run, and members that it does not read. So
// `mode` keeps the text it was written as, and unknown members are dropped rather than refused.

#pragma once

#include "pr/base.h"
#include "pr/byte_view.h"
#include "pr/compress.h"
#include "pr/string_view.h"

#include <stdint.h>
#include <string>
#include <vector>

PR_ASSUME_NONNULL_BEGIN

namespace alchemy {

// The keyword of the text chunk that holds the record.
constexpr PrStringView kProvenanceKeyword = "alchemy";

// An adapter of the chain that a run loaded, by the name that the server knew it under.
struct ProvenanceLora {
    std::string name;
    double weight = 1.0;
};

// One run's inputs. A record holds only the fields that its mode uses, so a field left at its default is one that the
// mode says nothing about. `source` is the file that the run worked from.
struct ProvenanceOp {
    std::string mode;
    std::string prompt;
    std::string negative;
    std::string checkpoint;
    std::vector<ProvenanceLora> loras;
    std::string sampler;
    std::string scheduler;
    std::string upscaleModel;
    std::string detector;
    std::string direction;
    std::string source;
    uint64_t seed = 0;
    uint32_t steps = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t batch = 0;
    uint32_t amount = 0;
    uint32_t crossfade = 0;
    uint32_t growMask = 0;
    uint32_t context = 0;
    double cfg = 0.0;
    double strength = 0.0;
    double factor = 0.0;
};

// The runs that made an image, oldest first.
struct ProvenanceRecord {
    std::vector<ProvenanceOp> operations;
};

// Returns the record as JSON. A seed is written as decimal text, since a JSON number cannot hold every 64-bit value.
std::string provenanceSerialize(const ProvenanceRecord& record);

// Reads the record that the JSON `text` holds into `out`. Returns false if the text is not a record.
bool provenanceParse(PrStringView text, ProvenanceRecord* out);

// Reads the record that the PNG file `png` carries into `out`, without decoding its pixels. `compress` reads a
// compressed chunk, and may be null. Returns false if the file carries no record.
bool provenanceRead(PrByteView png, PrCompressCodec* PR_NULLABLE compress, ProvenanceRecord* out);

// Returns the PNG file `png` with `record` in place of any record that it carried, or the empty string if `png` is not
// a PNG file.
std::string provenanceWrite(PrByteView png, const ProvenanceRecord& record);

// Returns the run that produced the image, or null if the record holds none.
const ProvenanceOp* PR_NULLABLE provenanceNewest(const ProvenanceRecord& record);

// Returns true if the operation sampled, and so recorded a model, prompts and sampler settings.
bool provenanceSamples(const ProvenanceOp& op);

// Returns the chain as one line, such as "generate → outpaint → upscale".
std::string provenanceChain(const ProvenanceRecord& record);

} // namespace alchemy

PR_ASSUME_NONNULL_END
