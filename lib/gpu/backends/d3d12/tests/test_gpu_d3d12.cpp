// The tests of what only the Direct3D 12 backend has: recording into the application's command lists and passes. The
// interface's own tests run on Direct3D 12 as well.

#include "pr/array.h"
#include "pr/gpu_d3d12.h"
#include "pr/mem.h"

#include "gpu/tests/kTestGpuNativeShaders.h"

#include "tools/harness/test.h"

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>

namespace pr_impl {

using namespace pr;

namespace {

// The root data of the test shaders, as test_gpu_native.slang declares it.
struct Push {
    uint32_t output;
    uint32_t count;
    uint32_t scale;
};

struct Uniforms {
    float color[4];
};

// A device on the system's high-performance adapter, with the debug layer where it is installed, and the test shaders.
// A test that finds no adapter with the features that the backend needs skips.
struct Fixture {
    PrGpuDevice* device = nullptr;
    PrGpuShaderLibrary* library = nullptr;

    Fixture() {
        PrGpuD3d12Options options = prGpuD3d12OptionsDefault();
        options.validation = true;
        PrStatus status = prGpuD3d12Create(&options, &device);
        if (status == PrStatus_NotSupported) {
            MESSAGE("no Direct3D 12 adapter with the features that the backend needs");
            return;
        }
        REQUIRE(status == PrStatus_Ok);
        REQUIRE(prGpuShaderLibraryCreate(device, kTestGpuNativeShaders, sizeof(kTestGpuNativeShaders), &library) ==
            PrStatus_Ok);
    }

    ~Fixture() {
        if (device == nullptr)
            return;
        prGpuShaderLibraryDestroy(library);
        prGpuDeviceDestroy(device);
    }

    PrGpuBuffer buffer(uint64_t size, PrGpuBufferUsageFlags usage, PrGpuMemory memory = PrGpuMemory_Device) {
        PrGpuBufferDesc desc{.size = size, .usage = usage, .memory = memory};
        PrGpuBuffer made{};
        REQUIRE(prGpuBufferCreate(device, &desc, &made) == PrStatus_Ok);
        return made;
    }

    PrGpuTexture target() {
        PrGpuTextureUsageFlags usage = PrGpuTextureUsageFlag_RenderTarget;
        PrGpuTextureDesc desc = prGpuTextureDesc2d(PrGpuFormat_Rgba8, PrUVec2{8, 8}, usage);
        PrGpuTexture texture{};
        REQUIRE(prGpuTextureCreate(device, &desc, &texture) == PrStatus_Ok);
        return texture;
    }

    PrGpuPipeline upperHalf() {
        PrGpuGraphicsPipelineDesc desc = prGpuGraphicsPipelineDescDefault(library);
        desc.vertex = "upperHalf";
        desc.fragment = "solid";
        desc.topology = PrGpuTopology_TriangleStrip;
        desc.formats.colorCount = 1;
        desc.formats.color[0] = PrGpuFormat_Rgba8;
        PrGpuPipeline pipeline{};
        REQUIRE(prGpuPipelineCreateGraphics(device, &desc, &pipeline) == PrStatus_Ok);
        return pipeline;
    }

    // Copies the words of `buffer` into readback memory, waits for them, and returns them.
    Array<uint32_t> words(PrGpuBuffer buffer, uint32_t count) {
        PrGpuBuffer readback = this->buffer(uint64_t(count) * 4, 0, PrGpuMemory_Readback);
        PrGpuCommands* commands = nullptr;
        REQUIRE(prGpuCommandsBegin(device, nullptr, &commands));
        prGpuCommandsBufferBarrier(commands, buffer, PrGpuAccessFlag_ShaderWrite | PrGpuAccessFlag_CopyWrite,
            PrGpuAccessFlag_CopyRead);
        prGpuCommandsCopyBuffer(commands, buffer, 0, readback, 0, uint64_t(count) * 4);
        prGpuDeviceWait(device, prGpuCommandsSubmit(commands));
        Array<uint32_t> out(count);
        prMemCopy(out.data(), prGpuBufferMap(device, readback), size_t(count) * 4);
        prGpuBufferDestroy(device, readback);
        return out;
    }
};

// An application's own use of Direct3D 12 on the device's ID3D12Device: a command list that it opens and closes, and a
// render target view of the texture that it renders into.
struct AppD3d12 {
    ID3D12Device4* device = nullptr;
    ID3D12CommandAllocator* allocator = nullptr;
    ID3D12GraphicsCommandList7* list = nullptr;
    ID3D12DescriptorHeap* targets = nullptr;

    explicit AppD3d12(PrGpuDevice* gpu) {
        ID3D12Device* base = static_cast<ID3D12Device*>(prGpuD3d12Device(gpu));
        REQUIRE(SUCCEEDED(base->QueryInterface(IID_PPV_ARGS(&device))));
        REQUIRE(SUCCEEDED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))));
        REQUIRE(SUCCEEDED(device->CreateCommandList1(0, D3D12_COMMAND_LIST_TYPE_DIRECT, D3D12_COMMAND_LIST_FLAG_NONE,
            IID_PPV_ARGS(&list))));
        REQUIRE(SUCCEEDED(list->Reset(allocator, nullptr)));
        D3D12_DESCRIPTOR_HEAP_DESC heap{.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV, .NumDescriptors = 1};
        REQUIRE(SUCCEEDED(device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&targets))));
    }

    ~AppD3d12() {
        targets->Release();
        list->Release();
        allocator->Release();
        device->Release();
    }

    // Returns a render target view of a texture, in the application's heap.
    D3D12_CPU_DESCRIPTOR_HANDLE view(ID3D12Resource* texture) {
        D3D12_CPU_DESCRIPTOR_HANDLE handle = targets->GetCPUDescriptorHandleForHeapStart();
        device->CreateRenderTargetView(texture, nullptr, handle);
        return handle;
    }
};

// Returns the texture's ID3D12Resource, its native object.
ID3D12Resource* nativeResource(PrGpuDevice* device, PrGpuTexture texture) {
    void* resource = nullptr;
    REQUIRE(prGpuTextureNative(device, texture, PrGpuNativeKind_D3d12, &resource));
    return static_cast<ID3D12Resource*>(resource);
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("d3d12 records into the application's command list, around the application's own work, and submits it") {
    Fixture f;
    if (f.device == nullptr)
        return;
    AppD3d12 app(f.device);

    // An upload made before the submission is visible to all of the submission's work, including the work in the
    // application's native command list.
    PrGpuBuffer output = f.buffer(16, PrGpuBufferUsageFlag_Storage);
    PrGpuPipeline pipeline{};
    REQUIRE(prGpuPipelineCreateCompute(f.device, f.library, "fill", "fill", &pipeline) == PrStatus_Ok);
    PrGpuCommandsOptions options = prGpuCommandsOptionsDefault();
    options.native = app.list;
    PrGpuCommands* commands = nullptr;
    REQUIRE(prGpuCommandsBegin(f.device, &options, &commands));
    uint32_t seed[4] = {9, 9, 9, 9};
    prGpuBufferUpload(f.device, output, 0, seed, sizeof(seed));
    Push push{.output = prGpuBufferStorageHandle(f.device, output), .count = 2, .scale = 11};
    prGpuCommandsSetPipeline(commands, pipeline);
    prGpuCommandsSetPush(commands, &push, sizeof(push));
    prGpuCommandsDispatch(commands, PrGpuExtent{1, 1, 1});
    PrGpuNativeStateFlags changed = prGpuNativeStateSync(commands);
    CHECK((changed & PrGpuNativeStateFlag_Bindings) != 0);
    CHECK((changed & PrGpuNativeStateFlag_Pipeline) != 0);
    CHECK((changed & PrGpuNativeStateFlag_Push) != 0);
    REQUIRE(SUCCEEDED(app.list->Close()));
    PrGpuFence fence = prGpuCommandsSubmit(commands);
    prGpuDeviceWait(f.device, fence);
    CHECK(prGpuDeviceIsComplete(f.device, fence));
    CHECK(f.words(output, 4) == Array<uint32_t>({0, 11, 9, 9}));
    prGpuPipelineDestroy(f.device, pipeline);
    prGpuBufferDestroy(f.device, output);
}

TEST_CASE("d3d12 draws into the application's pass, and says which of the pass's state its draws changed") {
    Fixture f;
    if (f.device == nullptr)
        return;
    AppD3d12 app(f.device);
    PrGpuTexture target = f.target();
    PrGpuPipeline pipeline = f.upperHalf();
    D3D12_CPU_DESCRIPTOR_HANDLE view = app.view(nativeResource(f.device, target));

    PrGpuCommandsOptions options = prGpuCommandsOptionsDefault();
    options.native = app.list;
    PrGpuCommands* commands = nullptr;
    REQUIRE(prGpuCommandsBegin(f.device, &options, &commands));
    prGpuCommandsTextureBarrier(commands, target, PrGpuSubresources{}, PrGpuAccessFlag_Discard,
        PrGpuAccessFlag_ColorAttachment);
    prGpuNativeStateSync(commands);

    // The application sets its render target and clears it to blue, and the device draws the upper half red into it.
    const float blue[4] = {0, 0, 1, 1};
    app.list->OMSetRenderTargets(1, &view, FALSE, nullptr);
    app.list->ClearRenderTargetView(view, blue, 0, nullptr);
    D3D12_VIEWPORT viewport{.Width = 8, .Height = 8, .MaxDepth = 1};
    D3D12_RECT scissor{.right = 8, .bottom = 8};
    app.list->RSSetViewports(1, &viewport);
    app.list->RSSetScissorRects(1, &scissor);
    PrGpuPassDesc passDesc{};
    passDesc.native = app.list;
    passDesc.nativeFormats.colorCount = 1;
    passDesc.nativeFormats.color[0] = PrGpuFormat_Rgba8;
    passDesc.nativeFormats.samples = 1;
    passDesc.nativeFormats.viewCount = 1;
    passDesc.nativeExtent = PrUVec2{8, 8};
    PrGpuPass* pass = prGpuPassBegin(commands, &passDesc);
    Uniforms uniforms{.color = {1, 0, 0, 1}};
    prGpuCommandsSetPipeline(commands, pipeline);
    prGpuCommandsSetUniforms(commands, &uniforms, sizeof(uniforms));
    prGpuPassDraw(pass, 4, 1, 0, 0);
    PrGpuNativeStateFlags changed = prGpuNativeStateSync(commands);
    CHECK((changed & PrGpuNativeStateFlag_Pipeline) != 0);
    CHECK((changed & PrGpuNativeStateFlag_Bindings) != 0);
    CHECK((changed & PrGpuNativeStateFlag_IndexBuffer) == 0);
    CHECK(prGpuNativeStateSync(commands) == 0);
    prGpuPassEnd(pass);

    prGpuCommandsTextureBarrier(commands, target, PrGpuSubresources{}, PrGpuAccessFlag_ColorAttachment,
        PrGpuAccessFlag_CopyRead);
    prGpuNativeStateSync(commands);
    REQUIRE(SUCCEEDED(app.list->Close()));
    prGpuDeviceWait(f.device, prGpuCommandsSubmit(commands));
    uint32_t pixels[8 * 8]{};
    prGpuTextureRead(f.device, target, PrGpuAccessFlag_CopyRead, PrIVec2{0, 0},
        PrPixels{reinterpret_cast<uint8_t*>(pixels), 8 * 4, PrUVec2{8, 8}});
    CHECK(pixels[0] == 0xff0000ffu);
    CHECK(pixels[7 * 8 + 7] == 0xffff0000u);
    prGpuPipelineDestroy(f.device, pipeline);
    prGpuTextureDestroy(f.device, target);
}

TEST_CASE("d3d12 hands a native draw the pass's command list, and draws after it with its own state again") {
    Fixture f;
    if (f.device == nullptr)
        return;
    REQUIRE(prGpuDeviceHasNative(f.device, PrGpuNativeKind_D3d12));
    PrGpuTexture target = f.target();
    PrGpuPipeline pipeline = f.upperHalf();
    PrGpuCommands* commands = nullptr;
    REQUIRE(prGpuCommandsBegin(f.device, nullptr, &commands));
    prGpuCommandsTextureBarrier(commands, target, PrGpuSubresources{}, PrGpuAccessFlag_Discard,
        PrGpuAccessFlag_ColorAttachment);
    PrGpuPassDesc desc = prGpuPassDescColor(target, PrGpuLoad_Clear, PrColor{0, 0, 1, 1});
    PrGpuPass* pass = prGpuPassBegin(commands, &desc);

    // The native draw narrows the scissor to the left half, which the device's draw after it does not keep.
    bool drawn = false;
    prGpuPassNativeDraw(pass, PrGpuNativeKind_D3d12, [](void* context, const void* native) {
        const PrGpuD3d12NativePass& objects = *static_cast<const PrGpuD3d12NativePass*>(native);
        D3D12_RECT left{0, 0, 4, 8};
        static_cast<ID3D12GraphicsCommandList7*>(objects.commandList)->RSSetScissorRects(1, &left);
        *static_cast<bool*>(context) = true;
    }, &drawn);
    CHECK(drawn);
    Uniforms uniforms{.color = {1, 0, 0, 1}};
    prGpuCommandsSetPipeline(commands, pipeline);
    prGpuCommandsSetUniforms(commands, &uniforms, sizeof(uniforms));
    prGpuPassDraw(pass, 4, 1, 0, 0);
    prGpuPassEnd(pass);
    prGpuCommandsTextureBarrier(commands, target, PrGpuSubresources{}, PrGpuAccessFlag_ColorAttachment,
        PrGpuAccessFlag_CopyRead);
    prGpuDeviceWait(f.device, prGpuCommandsSubmit(commands));
    uint32_t pixels[8 * 8]{};
    prGpuTextureRead(f.device, target, PrGpuAccessFlag_CopyRead, PrIVec2{0, 0},
        PrPixels{reinterpret_cast<uint8_t*>(pixels), 8 * 4, PrUVec2{8, 8}});
    CHECK(pixels[7] == 0xff0000ffu);
    CHECK(pixels[7 * 8] == 0xffff0000u);
    prGpuPipelineDestroy(f.device, pipeline);
    prGpuTextureDestroy(f.device, target);
}
