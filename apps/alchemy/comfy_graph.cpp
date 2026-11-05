// ComfyUI prompt graphs in the API format.

#include "apps/alchemy/comfy_graph.h"

#include "pr/assert.h"
#include "pr/string.h"

namespace alchemy {

void ComfyGraph::node(PrStringView id, PrStringView classType) {
    if (!started_) {
        prJsonWriterBeginObject(writer_);
        started_ = true;
    }
    if (nodeOpen_) {
        prJsonWriterEndObject(writer_);
        prJsonWriterEndObject(writer_);
    }
    prJsonWriterKey(writer_, id);
    prJsonWriterBeginObject(writer_);
    prJsonWriterKey(writer_, "class_type");
    prJsonWriterString(writer_, classType);
    prJsonWriterKey(writer_, "inputs");
    prJsonWriterBeginObject(writer_);
    nodeOpen_ = true;
}

void ComfyGraph::setText(PrStringView key, PrStringView value) {
    PR_ASSERT(nodeOpen_);
    prJsonWriterKey(writer_, key);
    prJsonWriterString(writer_, value);
}

void ComfyGraph::setNumber(PrStringView key, double value) {
    PR_ASSERT(nodeOpen_);
    prJsonWriterKey(writer_, key);
    prJsonWriterNumber(writer_, value);
}

void ComfyGraph::setBool(PrStringView key, bool value) {
    PR_ASSERT(nodeOpen_);
    prJsonWriterKey(writer_, key);
    prJsonWriterBool(writer_, value);
}

void ComfyGraph::setRef(PrStringView key, ComfyRef ref) {
    PR_ASSERT(nodeOpen_);
    prJsonWriterKey(writer_, key);
    prJsonWriterBeginArray(writer_);
    prJsonWriterString(writer_, ref.node);
    prJsonWriterNumber(writer_, double(ref.slot));
    prJsonWriterEndArray(writer_);
}

String ComfyGraph::build() {
    if (!started_) {
        prJsonWriterBeginObject(writer_);
        started_ = true;
    }
    if (nodeOpen_) {
        prJsonWriterEndObject(writer_);
        prJsonWriterEndObject(writer_);
        nodeOpen_ = false;
    }
    prJsonWriterEndObject(writer_);
    return prJsonWriterText(writer_);
}

} // namespace alchemy
