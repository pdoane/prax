// Models: what a model file states about the architecture it holds, and the models that alchemy can ask a server to
// download.
//
// The sizes that a checkpoint samples well at, and the adapters that it can load, follow from its architecture. So
// alchemy asks the file rather than reading its name. A safetensors file opens with a little-endian 64-bit byte count
// and then that many bytes of JSON, which name every tensor and, under `__metadata__`, what the writer recorded about
// the model. Detection reads that JSON alone, and never the weights behind it.
//
// Three sources answer, in falling order of confidence: the `modelspec.architecture` string that the reference tooling
// writes, the base model version that a Kohya training run records, and the shapes of the tensors. A UNet's
// cross-attention context width names its architecture, and a signature block name covers the architectures without a
// UNet. An adapter has no UNet of its own, so its width is read from the down projection of the cross-attention key
// that it adapts, which has the width of the layer that it is added to.

#pragma once

#include "pr/base.h"
#include "pr/byte_view.h"
#include "pr/string_view.h"

#include <stdint.h>
#include <string>
#include <vector>

PR_ASSUME_NONNULL_BEGIN

namespace alchemy {

// The diffusion model that a file holds. A refiner is reported apart from the base that it pairs with, since its
// cross-attention context differs.
enum {
    ModelArchitecture_Unknown,
    ModelArchitecture_Sd15,
    ModelArchitecture_Sdxl,
    ModelArchitecture_SdxlRefiner,
    ModelArchitecture_Sd3,
    ModelArchitecture_Flux,
    ModelArchitecture_Cascade,
};
typedef uint8_t ModelArchitecture;

// Returns the architecture's name as records spell it.
PrStringView architectureName(ModelArchitecture arch);

// Returns true if two files state architectures that cannot be used together. An unknown architecture claims nothing,
// so it conflicts with nothing. A refiner counts as the base that it pairs with, which its adapters are trained for.
bool architecturesConflict(ModelArchitecture a, ModelArchitecture b);

// Return the architecture that a safetensors file's JSON header names, that the leading bytes of a file name, or that
// the file at `path` names. Each returns Unknown if the bytes are not a safetensors header or name no architecture.
// Reading a file maps it, which reads only the pages of its header.
ModelArchitecture architectureFromHeader(PrStringView headerJson);
ModelArchitecture architectureFromPrefix(PrByteView leading);
ModelArchitecture readArchitecture(PrStringView path);

// The server folder that a kind of model lives in. The server reports the directories that it searches for each folder,
// so a client on the same machine opens a model's file through them.
enum {
    ModelFolder_Checkpoints,
    ModelFolder_Loras,
    ModelFolder_Count,
};
typedef uint8_t ModelFolder;

// Returns the folder's name as the server spells it.
PrStringView modelFolderName(ModelFolder folder);

// The vocabularies that a run cannot proceed without, each of which an essential model fills.
enum {
    EssentialKind_Checkpoint,
    EssentialKind_Upscale,
    EssentialKind_Count,
};
typedef uint8_t EssentialKind;

// A model that alchemy can ask the server to download. `type`, `base`, `filename` and `url` are ComfyUI-Manager's
// install vocabulary: `type` picks the destination folder, and `filename` is the name that the download is stored
// under, which the catalog then reports. `folder` is the destination as an instruction to install the file by hand
// spells it, for a server without the manager.
struct EssentialModel {
    EssentialKind kind;
    PrStringView name;
    PrStringView type;
    PrStringView base;
    PrStringView filename;
    PrStringView url;
    PrStringView folder;
};

// Returns the essential model of `kind`.
const EssentialModel& essentialModel(EssentialKind kind);

// Returns the essential model whose name or filename contains `name`, ignoring case, or null.
const EssentialModel* PR_NULLABLE findEssentialModel(PrStringView name);

} // namespace alchemy

PR_ASSUME_NONNULL_END
