// ComfyUI prompt graphs in the API format that the server's /prompt endpoint takes: an object that maps each node's id
// to its class and its inputs.

#pragma once

#include "pr/base.h"
#include "pr/json.h"
#include "pr/string_view.h"

#include <stdint.h>
#include <string>

PR_ASSUME_NONNULL_BEGIN

namespace alchemy {

// An output of a node: its id and the index of the output, which an input that it links to names.
struct ComfyRef {
    PrStringView node;
    uint32_t slot = 0;
};

// A graph under construction. Node ids are the caller's strings, so that a graph reads by stage ("latent", "sampler")
// and a progress event names the stage that it reached. Each input that `set` or `setRef` writes belongs to the node
// that the last `node` began.
class ComfyGraph {
public:
    ComfyGraph() = default;
    ~ComfyGraph() { prJsonWriterDestroy(writer_); }
    ComfyGraph(const ComfyGraph&) = delete;
    ComfyGraph& operator=(const ComfyGraph&) = delete;

    // Begins the node `id` of the class `classType`. Ids are unique within a graph.
    void node(PrStringView id, PrStringView classType);

    // Write an input of the open node: text, a number, a boolean, or a link to another node's output.
    void setText(PrStringView key, PrStringView value);
    void setNumber(PrStringView key, double value);
    void setBool(PrStringView key, bool value);
    void setRef(PrStringView key, ComfyRef ref);

    // Returns the graph's JSON text. No node or input may be added after.
    std::string build();

private:
    PrJsonWriter* writer_ = prJsonWriterCreate();
    bool started_ = false;      // the graph's object is open
    bool nodeOpen_ = false;
};

} // namespace alchemy

PR_ASSUME_NONNULL_END
