#include "pr/array.h"
#include "pr/gpu.h"
#include "pr/gpu_graph.h"
#include "pr/type_traits.h"

#include "gpu/tests/gpu_fixture.h"
#include "tools/harness/harness_gpu.h"

#include "tools/harness/test.h"

using namespace harness;

namespace pr_impl {

using namespace pr;

namespace {

constexpr PrUVec2 kExtent{4, 4};

// The bytes of the first level of a texture of kExtent in 8-bit RGBA.
constexpr uint64_t kTextureBytes = 4 * 4 * 4;

// Adds a pass or work whose callback is `call`, which the graph's memory holds for the frame.
template <typename Call>
Call* held(PrGpuGraph* graph, const Call& call) {
    static_assert(isTriviallyCopyable<Call> && alignof(Call) <= 16);
    Call* copy = static_cast<Call*>(prGpuGraphAlloc(graph, sizeof(Call)));
    prMemCopy(copy, &call, sizeof(Call));
    return copy;
}
template <typename Draw>
PrGpuGraphPass addPass(PrGpuGraph* graph, const PrGpuGraphPassDesc& desc, Draw draw) {
    return prGpuGraphAddPass(graph, &desc, [](void* PR_NULLABLE context, PrGpuPass* pass) {
        (*static_cast<Draw*>(context))(pass);
    }, held(graph, draw));
}
template <typename Record>
PrGpuGraphPass addWork(PrGpuGraph* graph, PrStringView name, Record record) {
    return prGpuGraphAddWork(graph, name, [](void* PR_NULLABLE context, PrGpuCommands* commands) {
        (*static_cast<Record*>(context))(commands);
    }, held(graph, record));
}

// A pass named `name` that clears and keeps `first` and, if it is given, `second`.
PrGpuGraphPassDesc clearing(PrStringView name, PrGpuGraphTexture first, PrGpuGraphTexture second = {}) {
    PrGpuGraphPassDesc desc = prGpuGraphPassDescColor(first, PrGpuLoad_Clear, PrColor{});
    desc.name = name;
    if (second.id != 0) {
        desc.colorCount = 2;
        desc.color[1] = {.texture = second, .load = PrGpuLoad_Clear, .store = PrGpuStore_Keep};
    }
    return desc;
}

// The description of a texture of the frame that passes draw to and sample, with `usage` besides.
PrGpuTextureDesc frameDesc(PrGpuTextureUsageFlags usage = 0) {
    return prGpuTextureDesc2d(PrGpuFormat_Rgba8, kExtent,
        PrGpuTextureUsageFlag_RenderTarget | PrGpuTextureUsageFlag_Sampled | usage);
}

// The names of the passes that a recording called, in order.
typedef Array<PrStringView> Calls;

// Adds a pass that clears `first` and `second`, or work, that appends its name to `calls` when it is recorded.
PrGpuGraphPass addNamedPass(PrGpuGraph* graph, Calls* calls, PrStringView name, PrGpuGraphTexture first,
        PrGpuGraphTexture second = {}) {
    return addPass(graph, clearing(name, first, second), [calls, name](PrGpuPass*) { calls->push(name); });
}
PrGpuGraphPass addNamedWork(PrGpuGraph* graph, Calls* calls, PrStringView name) {
    return addWork(graph, name, [calls, name](PrGpuCommands*) { calls->push(name); });
}

bool sameCalls(const Calls& calls, Span<const PrStringView> expected) {
    if (calls.count() != expected.count())
        return false;
    for (uint32_t i = 0; i < calls.count(); ++i) {
        if (!(calls[i] == expected[i]))
            return false;
    }
    return true;
}

// Records the graph into a command buffer of its own, and waits for the work.
PrGpuGraphStats record(Fixture& f, PrGpuGraph* graph) {
    PrGpuCommands* commands = nullptr;
    REQUIRE(prGpuCommandsBegin(f.device, nullptr, &commands));
    PrGpuGraphStats stats = prGpuGraphRecord(graph, commands);
    gpuSubmitAndWait(commands);
    return stats;
}

PrGpuGraph* createGraph(Fixture& f, PrGpuGraphPriority priority) {
    PrGpuGraphOptions options{.priority = priority};
    return prGpuGraphCreate(f.device, &options);
}

PrGpuBuffer createBuffer(Fixture& f, uint64_t size) {
    PrGpuBufferDesc desc{.size = size, .usage = PrGpuBufferUsageFlag_Storage};
    PrGpuBuffer buffer{};
    REQUIRE(prGpuBufferCreate(f.device, &desc, &buffer) == PrStatus_Ok);
    return buffer;
}

const PrGpuGraphPriority kPriorities[] = {PrGpuGraphPriority_Barriers, PrGpuGraphPriority_Memory};

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("a graph's default priority is the fewest barriers") {
    CHECK(prGpuGraphOptionsDefault().priority == PrGpuGraphPriority_Barriers);
    prGpuGraphDestroy(nullptr);
}

TEST_CASE("a frame texture used alike every frame needs no barrier from the second frame on, favoring barriers") {
    onEachBackend([](Fixture& f) {
        PrGpuTexture target = f.target(kExtent);
        PrGpuGraph* graph = createGraph(f, PrGpuGraphPriority_Barriers);
        PrGpuAccessFlags accesses = PrGpuAccessFlag_ShaderRead;
        for (uint32_t frame = 0; frame < 3; ++frame) {
            CAPTURE(frame);
            prGpuGraphBegin(graph);
            PrGpuTextureDesc desc = frameDesc();
            PrGpuGraphTexture color = prGpuGraphAddTexture(graph, &desc);
            PrGpuGraphTexture out = prGpuGraphImportTexture(graph, target, accesses);
            prGpuGraphLeaveTexture(graph, out, PrGpuAccessFlag_ColorAttachment);
            addPass(graph, clearing("draw", color, out), [](PrGpuPass*) {});
            PrGpuGraphStats stats = record(f, graph);
            CHECK(stats.passes == 1);
            CHECK(stats.textures == 1);
            CHECK(stats.textureBytes == kTextureBytes);
            // The first frame brings the new device texture and the target to the pass.
            CHECK(stats.barriers == (frame == 0 ? 2 : 0));
            CHECK(prGpuGraphErrorCount(graph) == 0);
            accesses = PrGpuAccessFlag_ColorAttachment;
        }
        prGpuGraphDestroy(graph);
        prGpuTextureRelease(f.device, target);
    });
}

TEST_CASE("frame textures whose passes do not overlap share one device texture, and pay a barrier, favoring memory") {
    onEachBackend([](Fixture& f) {
        PrGpuTexture targets[2] = {f.target(kExtent), f.target(kExtent)};
        PrGpuGraph* graph = createGraph(f, PrGpuGraphPriority_Memory);
        for (uint32_t frame = 0; frame < 3; ++frame) {
            CAPTURE(frame);
            prGpuGraphBegin(graph);
            PrGpuTextureDesc desc = frameDesc();
            for (PrGpuTexture target : targets) {
                PrGpuGraphTexture color = prGpuGraphAddTexture(graph, &desc);
                PrGpuGraphTexture out = prGpuGraphImportTexture(graph, target, PrGpuAccessFlag_ShaderRead);
                prGpuGraphLeaveTexture(graph, out, PrGpuAccessFlag_ShaderRead);
                addPass(graph, clearing("draw", color), [](PrGpuPass*) {});
                PrGpuGraphPass shown = addPass(graph, clearing("show", out), [](PrGpuPass*) {});
                prGpuGraphUseTexture(graph, shown, color, PrGpuAccessFlag_FragmentRead);
            }
            PrGpuGraphStats stats = record(f, graph);
            CHECK(stats.passes == 4);
            CHECK(stats.textures == 1);
            CHECK(stats.textureBytes == kTextureBytes);
            // Each target goes to the pass and back. Each frame texture finds the device texture in the accesses of a
            // read, and pays a barrier to draw to it and one to read it.
            CHECK(stats.barriers == 8);
        }
        prGpuGraphDestroy(graph);
        for (PrGpuTexture target : targets)
            prGpuTextureRelease(f.device, target);
    });
}

TEST_CASE("frame textures that end in different writes take a device texture each favoring barriers, and share one "
        "favoring memory") {
    onEachBackend([](Fixture& f) {
        for (PrGpuGraphPriority priority : kPriorities) {
            CAPTURE(priority);
            PrGpuTexture target = f.target(kExtent);
            PrGpuBuffer buffer = createBuffer(f, 64);
            PrGpuGraph* graph = createGraph(f, priority);
            PrGpuAccessFlags targetAccesses = PrGpuAccessFlag_ShaderRead;
            PrGpuAccessFlags bufferAccesses = PrGpuAccessFlag_ShaderRead;
            for (uint32_t frame = 0; frame < 3; ++frame) {
                CAPTURE(frame);
                prGpuGraphBegin(graph);
                PrGpuTextureDesc desc = frameDesc(PrGpuTextureUsageFlag_Storage);
                PrGpuGraphTexture drawn = prGpuGraphAddTexture(graph, &desc);
                PrGpuGraphTexture computed = prGpuGraphAddTexture(graph, &desc);
                PrGpuGraphTexture out = prGpuGraphImportTexture(graph, target, targetAccesses);
                PrGpuGraphBuffer result = prGpuGraphImportBuffer(graph, buffer, bufferAccesses);
                prGpuGraphLeaveTexture(graph, out, PrGpuAccessFlag_ColorAttachment);
                prGpuGraphLeaveBuffer(graph, result, PrGpuAccessFlag_ComputeWrite);
                addPass(graph, clearing("draw", drawn, out), [](PrGpuPass*) {});
                PrGpuGraphPass compute = addWork(graph, "compute", [](PrGpuCommands*) {});
                prGpuGraphUseTexture(graph, compute, computed, PrGpuAccessFlag_ComputeWrite);
                prGpuGraphUseBuffer(graph, compute, result, PrGpuAccessFlag_ComputeWrite);
                PrGpuGraphStats stats = record(f, graph);
                CHECK(stats.passes == 2);
                if (priority == PrGpuGraphPriority_Barriers) {
                    CHECK(stats.textures == 2);
                    CHECK(stats.barriers == (frame == 0 ? 4 : 0));
                } else {
                    CHECK(stats.textures == 1);
                    CHECK(stats.barriers == (frame == 0 ? 4 : 2));
                }
                targetAccesses = PrGpuAccessFlag_ColorAttachment;
                bufferAccesses = PrGpuAccessFlag_ComputeWrite;
            }
            prGpuGraphDestroy(graph);
            prGpuBufferRelease(f.device, buffer);
            prGpuTextureRelease(f.device, target);
        }
    });
}

TEST_CASE("a frame texture that ends in a read takes the same device texture every frame, under either priority") {
    onEachBackend([](Fixture& f) {
        for (PrGpuGraphPriority priority : kPriorities) {
            CAPTURE(priority);
            PrGpuTexture target = f.target(kExtent);
            PrGpuGraph* graph = createGraph(f, priority);
            uint32_t lent[4] = {};
            for (uint32_t frame = 0; frame < 4; ++frame) {
                prGpuGraphBegin(graph);
                PrGpuTextureDesc desc = frameDesc();
                PrGpuGraphTexture color = prGpuGraphAddTexture(graph, &desc);
                PrGpuGraphTexture out = prGpuGraphImportTexture(graph, target, PrGpuAccessFlag_ShaderRead);
                prGpuGraphLeaveTexture(graph, out, PrGpuAccessFlag_ShaderRead);
                addPass(graph, clearing("draw", color), [](PrGpuPass*) {});
                uint32_t* id = &lent[frame];
                PrGpuGraphPass shown = addPass(graph, clearing("show", out), [graph, color, id](PrGpuPass*) {
                    *id = prGpuGraphTexture(graph, color).id;
                });
                prGpuGraphUseTexture(graph, shown, color, PrGpuAccessFlag_FragmentRead);
                CHECK(prGpuGraphTexture(graph, color).id == 0);
                CHECK(prGpuGraphTexture(graph, out).id == target.id);
                PrGpuGraphStats stats = record(f, graph);
                CHECK(stats.textures == 1);
                CHECK(stats.barriers == 4);
                // The frame texture has its device texture only while its passes are recorded.
                CHECK(prGpuGraphTexture(graph, color).id == 0);
                CHECK(prGpuGraphTexture(graph, out).id == target.id);
            }
            CHECK(lent[0] != 0);
            CHECK(lent[1] == lent[0]);
            CHECK(lent[2] == lent[0]);
            CHECK(lent[3] == lent[0]);
            prGpuGraphDestroy(graph);
            prGpuTextureRelease(f.device, target);
        }
    });
}

TEST_CASE("a graph skips the passes that no result of the frame depends on") {
    onEachBackend([](Fixture& f) {
        PrGpuTexture target = f.target(kExtent);
        PrGpuGraph* graph = createGraph(f, PrGpuGraphPriority_Barriers);
        Calls calls;
        prGpuGraphBegin(graph);
        PrGpuTextureDesc desc = frameDesc();
        PrGpuGraphTexture used = prGpuGraphAddTexture(graph, &desc);
        PrGpuGraphTexture unused = prGpuGraphAddTexture(graph, &desc);
        PrGpuGraphTexture unusedSource = prGpuGraphAddTexture(graph, &desc);
        PrGpuGraphTexture followed = prGpuGraphAddTexture(graph, &desc);
        PrGpuGraphTexture out = prGpuGraphImportTexture(graph, target, PrGpuAccessFlag_ShaderRead);
        prGpuGraphLeaveTexture(graph, out, PrGpuAccessFlag_ShaderRead);
        addNamedPass(graph, &calls, "used", used);
        // Nothing reads what this pass draws, so neither it nor the pass that it reads is recorded.
        addNamedPass(graph, &calls, "unused source", unusedSource);
        PrGpuGraphPass skipped = addNamedPass(graph, &calls, "unused", unused);
        prGpuGraphUseTexture(graph, skipped, unusedSource, PrGpuAccessFlag_FragmentRead);
        // A recorded pass follows this pass, which keeps it.
        PrGpuGraphPass kept = addNamedPass(graph, &calls, "followed", followed);
        PrGpuGraphPass shown = addNamedPass(graph, &calls, "show", out);
        prGpuGraphUseTexture(graph, shown, used, PrGpuAccessFlag_FragmentRead);
        prGpuGraphFollowPass(graph, shown, kept);
        // Work that declares no write is always recorded.
        addNamedWork(graph, &calls, "effects");
        PrGpuGraphStats stats = record(f, graph);
        const PrStringView expected[] = {"used", "followed", "show", "effects"};
        CHECK(sameCalls(calls, expected));
        CHECK(stats.passes == 4);
        CHECK(stats.textures == 2);
        CHECK(prGpuGraphErrorCount(graph) == 0);
        prGpuGraphDestroy(graph);
        prGpuTextureRelease(f.device, target);
    });
}

TEST_CASE("writers of byte ranges that do not overlap need no barrier between them, and overlapping ones do") {
    onEachBackend([](Fixture& f) {
        PrGpuBuffer buffer = createBuffer(f, 64);
        PrGpuGraph* graph = createGraph(f, PrGpuGraphPriority_Barriers);
        const uint64_t secondOffsets[] = {16, 8};
        for (uint64_t secondOffset : secondOffsets) {
            CAPTURE(secondOffset);
            prGpuGraphBegin(graph);
            PrGpuGraphBuffer written = prGpuGraphImportBuffer(graph, buffer, PrGpuAccessFlag_ShaderRead);
            prGpuGraphLeaveBuffer(graph, written, PrGpuAccessFlag_ShaderRead);
            // The graph's buffer names the device's, and no buffer names none.
            CHECK(prGpuGraphBuffer(graph, written).id == buffer.id);
            CHECK(prGpuGraphBuffer(graph, PrGpuGraphBuffer{}).id == 0);
            PrGpuGraphPass first = addWork(graph, "first", [](PrGpuCommands*) {});
            PrGpuGraphPass second = addWork(graph, "second", [](PrGpuCommands*) {});
            prGpuGraphUseBufferRange(graph, first, written, PrGpuAccessFlag_ComputeWrite, 0, 16);
            prGpuGraphUseBufferRange(graph, second, written, PrGpuAccessFlag_ComputeWrite, secondOffset, 16);
            PrGpuGraphStats stats = record(f, graph);
            CHECK(stats.passes == 2);
            // One barrier before the first writer and one that leaves the buffer, and one between the writers if their
            // ranges overlap.
            CHECK(stats.barriers == (secondOffset == 16 ? 2 : 3));
        }
        prGpuGraphDestroy(graph);
        prGpuBufferRelease(f.device, buffer);
    });
}

TEST_CASE("passes are recorded in the order that they were added, under either priority") {
    onEachBackend([](Fixture& f) {
        for (PrGpuGraphPriority priority : kPriorities) {
            CAPTURE(priority);
            PrGpuTexture targets[2] = {f.target(kExtent), f.target(kExtent)};
            PrGpuBuffer buffer = createBuffer(f, 64);
            PrGpuGraph* graph = createGraph(f, priority);
            Calls calls;
            prGpuGraphBegin(graph);
            PrGpuTextureDesc desc = frameDesc();
            PrGpuGraphTexture colors[2] = {prGpuGraphAddTexture(graph, &desc), prGpuGraphAddTexture(graph, &desc)};
            PrGpuGraphTexture outs[2] = {};
            for (uint32_t i = 0; i < 2; ++i) {
                outs[i] = prGpuGraphImportTexture(graph, targets[i], PrGpuAccessFlag_ShaderRead);
                prGpuGraphLeaveTexture(graph, outs[i], PrGpuAccessFlag_ShaderRead);
            }
            PrGpuGraphBuffer written = prGpuGraphImportBuffer(graph, buffer, PrGpuAccessFlag_ShaderRead);
            prGpuGraphLeaveBuffer(graph, written, PrGpuAccessFlag_ShaderRead);

            // Both frame textures are drawn before either is shown, so each needs a device texture of its own.
            addNamedPass(graph, &calls, "draw 0", colors[0]);
            addNamedPass(graph, &calls, "draw 1", colors[1]);
            PrGpuGraphPass show0 = addNamedPass(graph, &calls, "show 0", outs[0]);
            PrGpuGraphPass show1 = addNamedPass(graph, &calls, "show 1", outs[1]);
            prGpuGraphUseTexture(graph, show0, colors[0], PrGpuAccessFlag_FragmentRead);
            prGpuGraphUseTexture(graph, show1, colors[1], PrGpuAccessFlag_FragmentRead);
            // A copy lies between two dispatches that write other ranges of the buffer, so each of the three needs a
            // barrier.
            PrGpuGraphPass compute0 = addNamedWork(graph, &calls, "compute 0");
            PrGpuGraphPass copy = addNamedWork(graph, &calls, "copy");
            PrGpuGraphPass compute1 = addNamedWork(graph, &calls, "compute 1");
            prGpuGraphUseBufferRange(graph, compute0, written, PrGpuAccessFlag_ComputeWrite, 0, 16);
            prGpuGraphUseBufferRange(graph, copy, written, PrGpuAccessFlag_CopyWrite, 16, 16);
            prGpuGraphUseBufferRange(graph, compute1, written, PrGpuAccessFlag_ComputeWrite, 32, 16);

            PrGpuGraphStats stats = record(f, graph);
            CHECK(stats.passes == 7);
            CHECK(prGpuGraphErrorCount(graph) == 0);
            const PrStringView expected[] = {"draw 0", "draw 1", "show 0", "show 1", "compute 0", "copy", "compute 1"};
            CHECK(sameCalls(calls, expected));
            CHECK(stats.textures == 2);
            // Two barriers for each frame texture and for each target, and four for the buffer.
            CHECK(stats.barriers == 12);
            prGpuGraphDestroy(graph);
            prGpuBufferRelease(f.device, buffer);
            for (PrGpuTexture target : targets)
                prGpuTextureRelease(f.device, target);
        }
    });
}

TEST_CASE("a pass is recorded after the passes that it follows, and after the writers of what it uses") {
    onEachBackend([](Fixture& f) {
        for (PrGpuGraphPriority priority : kPriorities) {
            CAPTURE(priority);
            PrGpuBuffer buffer = createBuffer(f, 64);
            PrGpuGraph* graph = createGraph(f, priority);
            Calls calls;
            prGpuGraphBegin(graph);
            PrGpuGraphBuffer shared = prGpuGraphImportBuffer(graph, buffer, PrGpuAccessFlag_ShaderRead);
            prGpuGraphLeaveBuffer(graph, shared, PrGpuAccessFlag_ShaderRead);
            PrGpuGraphPass first = addNamedWork(graph, &calls, "first");
            PrGpuGraphPass reader = addNamedWork(graph, &calls, "reader");
            PrGpuGraphPass writer = addNamedWork(graph, &calls, "writer");
            PrGpuGraphPass last = addNamedWork(graph, &calls, "last");
            // The writer follows the reader of the bytes that it writes over, and the first pass follows the last.
            prGpuGraphUseBuffer(graph, reader, shared, PrGpuAccessFlag_ComputeRead);
            prGpuGraphUseBuffer(graph, writer, shared, PrGpuAccessFlag_ComputeWrite);
            prGpuGraphFollowPass(graph, first, last);
            record(f, graph);
            const PrStringView expected[] = {"reader", "writer", "last", "first"};
            CHECK(sameCalls(calls, expected));
            CHECK(prGpuGraphErrorCount(graph) == 0);
            prGpuGraphDestroy(graph);
            prGpuBufferRelease(f.device, buffer);
        }
    });
}

TEST_CASE("a graph reports a pass that reads and writes one resource") {
    onEachBackend([](Fixture& f) {
        PrGpuTexture target = f.target(kExtent);
        PrGpuBuffer buffer = createBuffer(f, 64);
        PrGpuGraph* graph = createGraph(f, PrGpuGraphPriority_Barriers);
        prGpuGraphBegin(graph);
        PrGpuGraphTexture out = prGpuGraphImportTexture(graph, target, PrGpuAccessFlag_ShaderRead);
        PrGpuGraphBuffer shared = prGpuGraphImportBuffer(graph, buffer, PrGpuAccessFlag_ShaderRead);
        prGpuGraphLeaveTexture(graph, out, PrGpuAccessFlag_ShaderRead);
        prGpuGraphLeaveBuffer(graph, shared, PrGpuAccessFlag_ShaderRead);
        PrGpuGraphPass drawn = addPass(graph, clearing("feedback", out), [](PrGpuPass*) {});
        CHECK(prGpuGraphErrorCount(graph) == 0);
        prGpuGraphUseTexture(graph, drawn, out, PrGpuAccessFlag_FragmentRead);
        REQUIRE(prGpuGraphErrorCount(graph) == 1);
        PrGpuGraphError error = prGpuGraphError(graph, 0);
        CHECK(error.kind == PrGpuGraphErrorKind_ReadWrite);
        CHECK(error.pass.id == drawn.id);
        CHECK(error.name == "feedback");
        // A use that is declared again in the same accesses is no error, and a pass is reported once for a resource.
        prGpuGraphUseTexture(graph, drawn, out, PrGpuAccessFlag_FragmentRead);
        PrGpuGraphPass work = addWork(graph, "in place", [](PrGpuCommands*) {});
        prGpuGraphUseBuffer(graph, work, shared, PrGpuAccessFlag_ComputeWrite);
        prGpuGraphUseBuffer(graph, work, shared, PrGpuAccessFlag_ComputeWrite);
        CHECK(prGpuGraphErrorCount(graph) == 1);
        prGpuGraphUseBuffer(graph, work, shared, PrGpuAccessFlag_ComputeRead);
        REQUIRE(prGpuGraphErrorCount(graph) == 2);
        CHECK(prGpuGraphError(graph, 1).kind == PrGpuGraphErrorKind_ReadWrite);
        CHECK(prGpuGraphError(graph, 1).name == "in place");
        // The next frame begins without errors.
        prGpuGraphBegin(graph);
        CHECK(prGpuGraphErrorCount(graph) == 0);
        prGpuGraphDestroy(graph);
        prGpuBufferRelease(f.device, buffer);
        prGpuTextureRelease(f.device, target);
    });
}

TEST_CASE("a graph reports a read of a frame texture that no pass has written") {
    onEachBackend([](Fixture& f) {
        PrGpuTexture target = f.target(kExtent);
        PrGpuGraph* graph = createGraph(f, PrGpuGraphPriority_Barriers);
        prGpuGraphBegin(graph);
        PrGpuTextureDesc desc = frameDesc();
        PrGpuGraphTexture color = prGpuGraphAddTexture(graph, &desc);
        PrGpuGraphTexture out = prGpuGraphImportTexture(graph, target, PrGpuAccessFlag_ShaderRead);
        prGpuGraphLeaveTexture(graph, out, PrGpuAccessFlag_ShaderRead);
        PrGpuGraphPass shown = addPass(graph, clearing("show", out), [](PrGpuPass*) {});
        prGpuGraphUseTexture(graph, shown, color, PrGpuAccessFlag_FragmentRead);
        // The pass that draws the texture comes too late for the read.
        PrGpuGraphPass late = addPass(graph, clearing("late", color), [](PrGpuPass*) {});
        PrGpuGraphPass after = addWork(graph, "after", [](PrGpuCommands*) {});
        prGpuGraphFollowPass(graph, after, late);
        CHECK(prGpuGraphErrorCount(graph) == 0);
        PrGpuGraphStats stats = record(f, graph);
        CHECK(stats.passes == 3);
        REQUIRE(prGpuGraphErrorCount(graph) == 1);
        CHECK(prGpuGraphError(graph, 0).kind == PrGpuGraphErrorKind_UnwrittenRead);
        CHECK(prGpuGraphError(graph, 0).pass.id == shown.id);
        CHECK(prGpuGraphError(graph, 0).name == "show");
        prGpuGraphDestroy(graph);
        prGpuTextureRelease(f.device, target);
    });
}

TEST_CASE("a graph reports a cycle of passes that follow each other, and records them all") {
    onEachBackend([](Fixture& f) {
        for (PrGpuGraphPriority priority : kPriorities) {
            CAPTURE(priority);
            PrGpuGraph* graph = createGraph(f, priority);
            Calls calls;
            prGpuGraphBegin(graph);
            PrGpuGraphPass before = addNamedWork(graph, &calls, "before");
            PrGpuGraphPass a = addNamedWork(graph, &calls, "a");
            PrGpuGraphPass b = addNamedWork(graph, &calls, "b");
            PrGpuGraphPass c = addNamedWork(graph, &calls, "c");
            prGpuGraphFollowPass(graph, a, before);
            prGpuGraphFollowPass(graph, a, c);
            prGpuGraphFollowPass(graph, b, a);
            prGpuGraphFollowPass(graph, c, b);
            PrGpuGraphStats stats = record(f, graph);
            CHECK(stats.passes == 4);
            // The earliest added pass of the cycle is recorded ahead of the passes that it follows.
            const PrStringView expected[] = {"before", "a", "b", "c"};
            CHECK(sameCalls(calls, expected));
            REQUIRE(prGpuGraphErrorCount(graph) == 1);
            CHECK(prGpuGraphError(graph, 0).kind == PrGpuGraphErrorKind_Cycle);
            CHECK(prGpuGraphError(graph, 0).pass.id == a.id);
            CHECK(prGpuGraphError(graph, 0).name == "a");
            prGpuGraphDestroy(graph);
        }
    });
}

TEST_CASE("a graph reports and ignores a texture, a buffer or a pass of an earlier frame") {
    onEachBackend([](Fixture& f) {
        PrGpuTexture target = f.target(kExtent);
        PrGpuBuffer buffer = createBuffer(f, 64);
        PrGpuGraph* graph = createGraph(f, PrGpuGraphPriority_Barriers);
        prGpuGraphBegin(graph);
        PrGpuGraphTexture staleTexture = prGpuGraphImportTexture(graph, target, PrGpuAccessFlag_ShaderRead);
        PrGpuGraphBuffer staleBuffer = prGpuGraphImportBuffer(graph, buffer, PrGpuAccessFlag_ShaderRead);
        PrGpuGraphPass stalePass = addWork(graph, "stale", [](PrGpuCommands*) {});
        record(f, graph);

        Calls calls;
        prGpuGraphBegin(graph);
        PrGpuGraphTexture out = prGpuGraphImportTexture(graph, target, PrGpuAccessFlag_ShaderRead);
        PrGpuGraphBuffer shared = prGpuGraphImportBuffer(graph, buffer, PrGpuAccessFlag_ShaderRead);
        PrGpuGraphPass work = addNamedWork(graph, &calls, "work");
        CHECK(staleTexture.id != out.id);
        CHECK(prGpuGraphTexture(graph, staleTexture).id == 0);
        // None is ignored without an error.
        prGpuGraphUseTexture(graph, work, PrGpuGraphTexture{}, PrGpuAccessFlag_ComputeRead);
        prGpuGraphUseBuffer(graph, PrGpuGraphPass{}, shared, PrGpuAccessFlag_ComputeRead);
        CHECK(prGpuGraphErrorCount(graph) == 0);

        prGpuGraphUseTexture(graph, work, staleTexture, PrGpuAccessFlag_ComputeWrite);
        prGpuGraphUseBuffer(graph, work, staleBuffer, PrGpuAccessFlag_ComputeWrite);
        prGpuGraphUseBufferRange(graph, stalePass, shared, PrGpuAccessFlag_ComputeWrite, 0, 16);
        prGpuGraphLeaveTexture(graph, staleTexture, PrGpuAccessFlag_Present);
        prGpuGraphLeaveBuffer(graph, staleBuffer, PrGpuAccessFlag_CopyRead);
        prGpuGraphFollowPass(graph, work, stalePass);
        // A pass that is given a stale attachment has none, so it is not recorded.
        addNamedPass(graph, &calls, "drawn", staleTexture);
        REQUIRE(prGpuGraphErrorCount(graph) == 7);
        for (uint32_t i = 0; i < 7; ++i)
            CHECK(prGpuGraphError(graph, i).kind == PrGpuGraphErrorKind_StaleId);
        CHECK(prGpuGraphError(graph, 0).pass.id == work.id);
        CHECK(prGpuGraphError(graph, 0).name == "work");
        CHECK(prGpuGraphError(graph, 2).pass.id == 0);
        CHECK(prGpuGraphError(graph, 6).name == "drawn");

        PrGpuGraphStats stats = record(f, graph);
        // The ignored calls declared no use, so the work wrote nothing and no barrier was recorded.
        CHECK(stats.passes == 1);
        CHECK(stats.barriers == 0);
        const PrStringView expected[] = {"work"};
        CHECK(sameCalls(calls, expected));
        prGpuGraphDestroy(graph);
        prGpuBufferRelease(f.device, buffer);
        prGpuTextureRelease(f.device, target);
    });
}

TEST_CASE("a graph's memory is aligned, apart, and reused by the next frame") {
    onEachBackend([](Fixture& f) {
        PrGpuGraph* graph = createGraph(f, PrGpuGraphPriority_Barriers);
        // The fourth is larger than any page of the frame's storage. It has a block of its own, which the next frame
        // frees instead of reusing.
        const size_t kLarge = 100000;
        const size_t sizes[] = {1, 0, 24, kLarge, 16};
        uint8_t* first[5] = {};
        for (uint32_t frame = 0; frame < 2; ++frame) {
            prGpuGraphBegin(graph);
            uint8_t* allocated[5] = {};
            for (uint32_t i = 0; i < 5; ++i) {
                uint8_t* bytes = static_cast<uint8_t*>(prGpuGraphAlloc(graph, sizes[i]));
                REQUIRE(bytes != nullptr);
                CHECK(uintptr_t(bytes) % 16 == 0);
                for (size_t at = 0; at < sizes[i]; ++at)
                    bytes[at] = uint8_t(i + 1);
                allocated[i] = bytes;
                if (frame == 0)
                    first[i] = bytes;
                else if (sizes[i] != kLarge)
                    CHECK(bytes == first[i]);
            }
            bool intact = true;
            for (uint32_t i = 0; i < 5; ++i) {
                for (size_t at = 0; at < sizes[i]; ++at)
                    intact = intact && allocated[i][at] == uint8_t(i + 1);
            }
            CHECK(intact);
        }
        prGpuGraphDestroy(graph);
    });
}

TEST_CASE("a graph's passes draw what the passes before them drew, through frame textures") {
    onEachBackend([](Fixture& f) {
        for (PrGpuGraphPriority priority : kPriorities) {
            CAPTURE(priority);
            PrGpuTexture targets[2] = {f.target(kExtent), f.target(kExtent)};
            PrGpuPipeline solid = f.pipeline("quadVertex", "solid");
            PrGpuPipeline sampled = f.pipeline("quadVertex", "slotSampled");
            PrGpuPipeline filled{};
            REQUIRE(prGpuPipelineCreateCompute(f.device, f.library, "slotFilled", "slotFilled", &filled) ==
                PrStatus_Ok);
            PrGpuSampler nearest{};
            PrGpuSamplerDesc samplerDesc = prGpuSamplerDescDefault(PrGpuFilter_Nearest, PrGpuAddress_Clamp);
            REQUIRE(prGpuSamplerIntern(f.device, &samplerDesc, &nearest) == PrStatus_Ok);
            PrGpuGraph* graph = createGraph(f, priority);

            // A pass that draws the whole of `source` into its target.
            struct Shown {
                PrGpuGraph* graph;
                PrGpuGraphTexture source;
                PrGpuPipeline pipeline;
                PrGpuSampler sampler;
            };
            PrGpuGraphDrawFn show = [](void* PR_NULLABLE context, PrGpuPass* pass) {
                const Shown& shown = *static_cast<const Shown*>(context);
                PrGpuCommands* commands = prGpuPassCommands(pass);
                Push push = fill(0, 0, 0, 0);
                prGpuCommandsSetTexture(commands, 2, prGpuGraphTexture(shown.graph, shown.source), shown.sampler);
                prGpuCommandsSetPipeline(commands, shown.pipeline);
                prGpuCommandsSetPush(commands, &push, sizeof(push));
                prGpuPassDraw(pass, 4, 1, 0, 0);
                prGpuCommandsSetTexture(commands, 2, PrGpuTexture{}, PrGpuSampler{});
            };

            for (uint32_t frame = 0; frame < 2; ++frame) {
                CAPTURE(frame);
                prGpuGraphBegin(graph);
                PrGpuTextureDesc desc = frameDesc(PrGpuTextureUsageFlag_Storage);
                PrGpuGraphTexture drawn = prGpuGraphAddTexture(graph, &desc);
                PrGpuGraphTexture copied = prGpuGraphAddTexture(graph, &desc);
                PrGpuGraphTexture computed = prGpuGraphAddTexture(graph, &desc);
                PrGpuGraphTexture unused = prGpuGraphAddTexture(graph, &desc);
                PrGpuGraphTexture outs[2] = {};
                for (uint32_t i = 0; i < 2; ++i) {
                    outs[i] = prGpuGraphImportTexture(graph, targets[i], PrGpuAccessFlag_ShaderRead);
                    prGpuGraphLeaveTexture(graph, outs[i], PrGpuAccessFlag_ShaderRead);
                }

                // Red, with green over the left half.
                PrGpuGraphPassDesc drawDesc = clearing("draw", drawn);
                drawDesc.clearColor[0] = PrColor{1, 0, 0, 1};
                addPass(graph, drawDesc, [solid](PrGpuPass* pass) {
                    PrGpuCommands* commands = prGpuPassCommands(pass);
                    Push push{.color = {0, 1, 0, 1}, .rect = {-1, 1, 0, -1}};
                    prGpuCommandsSetPipeline(commands, solid);
                    prGpuCommandsSetPush(commands, &push, sizeof(push));
                    prGpuPassDraw(pass, 4, 1, 0, 0);
                });
                // A pass that nothing depends on would draw white over the first target.
                addPass(graph, clearing("unused", unused), [](PrGpuPass*) { FAIL("an unused pass was recorded"); });
                Shown* copy = held(graph, Shown{graph, drawn, sampled, nearest});
                PrGpuGraphPassDesc copyDesc = clearing("copy", copied);
                PrGpuGraphPass copies = prGpuGraphAddPass(graph, &copyDesc, show, copy);
                prGpuGraphUseTexture(graph, copies, drawn, PrGpuAccessFlag_FragmentRead);
                Shown* shown = held(graph, Shown{graph, copied, sampled, nearest});
                PrGpuGraphPassDesc showDesc = clearing("show", outs[0]);
                PrGpuGraphPass shows = prGpuGraphAddPass(graph, &showDesc, show, shown);
                prGpuGraphUseTexture(graph, shows, copied, PrGpuAccessFlag_FragmentRead);

                // Blue, from a dispatch.
                PrGpuGraphPass fills = addWork(graph, "fill", [graph, computed, filled](PrGpuCommands* commands) {
                    Push push = fill(0, 0, 1, 1);
                    prGpuCommandsSetPush(commands, &push, sizeof(push));
                    prGpuCommandsSetRwTexture(commands, 2, prGpuGraphTexture(graph, computed));
                    prGpuCommandsSetPipeline(commands, filled);
                    prGpuCommandsDispatch(commands, PrGpuExtent{1, 1, 1});
                    prGpuCommandsSetRwTexture(commands, 2, PrGpuTexture{});
                });
                prGpuGraphUseTexture(graph, fills, computed, PrGpuAccessFlag_ComputeWrite);
                Shown* filledShown = held(graph, Shown{graph, computed, sampled, nearest});
                PrGpuGraphPassDesc showFilledDesc = clearing("show filled", outs[1]);
                PrGpuGraphPass showsFilled = prGpuGraphAddPass(graph, &showFilledDesc, show, filledShown);
                prGpuGraphUseTexture(graph, showsFilled, computed, PrGpuAccessFlag_FragmentRead);

                PrGpuGraphStats stats = record(f, graph);
                CHECK(stats.passes == 5);
                CHECK(prGpuGraphErrorCount(graph) == 0);
                Array<uint8_t> out = f.read(targets[0]);
                CHECK(near(texel(out, 4, 0, 0), 0, 255, 0, 255));
                CHECK(near(texel(out, 4, 1, 3), 0, 255, 0, 255));
                CHECK(near(texel(out, 4, 2, 0), 255, 0, 0, 255));
                CHECK(near(texel(out, 4, 3, 3), 255, 0, 0, 255));
                out = f.read(targets[1]);
                CHECK(near(texel(out, 4, 0, 0), 0, 0, 255, 255));
                CHECK(near(texel(out, 4, 3, 3), 0, 0, 255, 255));
            }
            prGpuGraphDestroy(graph);
            prGpuPipelineRelease(f.device, filled);
            prGpuPipelineRelease(f.device, sampled);
            prGpuPipelineRelease(f.device, solid);
            for (PrGpuTexture target : targets)
                prGpuTextureRelease(f.device, target);
        }
    });
}

TEST_CASE("a graph on a lost device records no pass that needs a texture that it cannot lend, and stays safe") {
    onEachBackend([](Fixture& f) {
        PrGpuTexture target = f.target(kExtent);
        PrGpuGraph* graph = createGraph(f, PrGpuGraphPriority_Barriers);
        PrGpuCommands* commands = nullptr;
        REQUIRE(prGpuCommandsBegin(f.device, nullptr, &commands));
        prGpuDeviceSimulateLoss(f.device);
        Calls calls;
        for (uint32_t frame = 0; frame < 2; ++frame) {
            calls.clear();
            prGpuGraphBegin(graph);
            PrGpuTextureDesc desc = frameDesc();
            PrGpuGraphTexture color = prGpuGraphAddTexture(graph, &desc);
            PrGpuGraphTexture out = prGpuGraphImportTexture(graph, target, PrGpuAccessFlag_ShaderRead);
            // A texture that the caller could not make on the lost device.
            PrGpuGraphTexture absent = prGpuGraphImportTexture(graph, PrGpuTexture{}, PrGpuAccessFlag_ShaderRead);
            prGpuGraphLeaveTexture(graph, out, PrGpuAccessFlag_ShaderRead);
            prGpuGraphLeaveTexture(graph, absent, PrGpuAccessFlag_Present);
            addNamedPass(graph, &calls, "draw", color);
            PrGpuGraphPass shown = addNamedPass(graph, &calls, "show", out);
            prGpuGraphUseTexture(graph, shown, color, PrGpuAccessFlag_FragmentRead);
            addNamedPass(graph, &calls, "absent", absent);
            addNamedWork(graph, &calls, "effects");
            if (frame == 1)
                break;
            PrGpuGraphStats stats = prGpuGraphRecord(graph, commands);
            const PrStringView expected[] = {"effects"};
            CHECK(sameCalls(calls, expected));
            CHECK(stats.passes == 1);
            CHECK(stats.textures == 0);
            CHECK(stats.barriers == 0);
            CHECK(prGpuGraphTexture(graph, color).id == 0);
            prGpuCommandsSubmit(commands);
        }
        // A graph is destroyed with a frame that was never recorded.
        prGpuGraphDestroy(graph);
        prGpuTextureRelease(f.device, target);
    });
}
