// The perspective example's pipelines on a lib/gpu device. Each pipeline is made for the format of the passes that draw
// it, on first use.

#include "examples/perspective/perspective_gpu.h"

#include "examples/perspective/kPerspectiveShaders.h"

#include <cstdio>
#include <vector>

namespace perspective {

// A pipeline of one fragment shader for targets of one format.
struct Pipeline {
    const char* fragment;
    PrGpuFormat format;
    PrGpuPipeline pipeline;
};

struct Pipelines {
    PrGpuDevice* device;
    PrGpuShaderLibrary* library;
    std::vector<Pipeline> made;
};

namespace {

// Returns the pipeline of `fragment` for the pass's format, made on first use, or zero if the device cannot make it.
PrGpuPipeline pipelineFor(Pipelines* p, const char* fragment, PrGpuPass* pass) {
    PrGpuFormat format = prGpuPassFormats(pass).color[0];
    for (const Pipeline& made : p->made) {
        if (made.fragment == fragment && made.format == format)
            return made.pipeline;
    }
    PrGpuGraphicsPipelineDesc desc = prGpuGraphicsPipelineDescDefault(p->library);
    desc.vertex = "wholeVertex";
    desc.fragment = fragment;
    desc.formats.colorCount = 1;
    desc.formats.color[0] = format;
    PrGpuPipeline pipeline{};
    if (prGpuPipelineCreateGraphics(p->device, &desc, &pipeline) != PrStatus_Ok)
        fprintf(stderr, "perspective: the device makes no pipeline of %s\n", fragment);
    p->made.push_back(Pipeline{fragment, format, pipeline});
    return pipeline;
}

} // namespace

Pipelines* PR_NULLABLE pipelinesCreate(PrGpuDevice* device) {
    Pipelines* p = new Pipelines{};
    p->device = device;
    if (prGpuShaderLibraryCreate(device, kPerspectiveShaders, sizeof(kPerspectiveShaders), &p->library)
        != PrStatus_Ok) {
        fprintf(stderr, "perspective: the device takes no shaders of the example's\n");
        delete p;
        return nullptr;
    }
    return p;
}

void pipelinesDestroy(Pipelines* PR_NULLABLE pipelines) {
    if (pipelines == nullptr)
        return;
    for (const Pipeline& made : pipelines->made)
        prGpuPipelineDestroy(pipelines->device, made.pipeline);
    prGpuShaderLibraryDestroy(pipelines->library);
    delete pipelines;
}

void pipelinesDrawBackground(Pipelines* pipelines, PrGpuPass* pass, const BackgroundArguments& arguments) {
    PrGpuPipeline pipeline = pipelineFor(pipelines, "backgroundPixel", pass);
    if (pipeline.id == 0)
        return;
    PrGpuCommands* commands = prGpuPassCommands(pass);
    PrUVec2 extent = prGpuPassExtent(pass);
    prGpuCommandsSetPipeline(commands, pipeline);
    prGpuCommandsSetUniforms(commands, &arguments, sizeof(arguments));
    prGpuPassSetViewport(pass, PrRect{0, 0, float(extent.x), float(extent.y)}, 0, 1);
    prGpuPassDraw(pass, 3, 1, 0, 0);
}

void pipelinesCopy(Pipelines* pipelines, PrGpuPass* pass, PrGpuTexture scene) {
    PrGpuPipeline pipeline = pipelineFor(pipelines, "copyPixel", pass);
    if (pipeline.id == 0)
        return;
    PrGpuCommands* commands = prGpuPassCommands(pass);
    prGpuCommandsSetPipeline(commands, pipeline);
    prGpuCommandsSetPush(commands, &scene.id, sizeof(scene.id));
    prGpuPassDraw(pass, 3, 1, 0, 0);
}

} // namespace perspective
