// The tests of what only the Direct3D 12 backend has: recording into the host's command lists and passes. The
// interface's own tests run on Direct3D 12 as well.

#include "pr/gpu_d3d12.h"

#include "gpu/tests/kTestGpuHostShaders.h"

#include "tools/harness/test.h"

#include <cstdint>
#include <cstring>
#include <vector>

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>

namespace pr_impl {

namespace {

// The root data of the test shaders, as test_gpu_host.slang declares it.
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
        REQUIRE(prGpuShaderLibraryCreate(device, kTestGpuHostShaders, sizeof(kTestGpuHostShaders), &library) ==
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
    std::vector<uint32_t> words(PrGpuBuffer buffer, uint32_t count) {
        PrGpuBuffer readback = this->buffer(uint64_t(count) * 4, 0, PrGpuMemory_Readback);
        PrGpuCommands* commands = nullptr;
        REQUIRE(prGpuCommandsBegin(device, nullptr, &commands));
        prGpuCommandsBufferBarrier(commands, buffer, PrGpuAccessFlag_ShaderWrite | PrGpuAccessFlag_CopyWrite,
            PrGpuAccessFlag_CopyRead);
        prGpuCommandsCopyBuffer(commands, buffer, 0, readback, 0, uint64_t(count) * 4);
        prGpuDeviceWait(device, prGpuCommandsSubmit(commands));
        std::vector<uint32_t> out(count);
        memcpy(out.data(), prGpuBufferMap(device, readback), size_t(count) * 4);
        prGpuBufferDestroy(device, readback);
        return out;
    }
};

// A host's own use of Direct3D 12 on the device's ID3D12Device: a command list that it opens and closes, and a render
// target view of the texture that it renders into.
struct HostD3d12 {
    ID3D12Device4* device = nullptr;
    ID3D12CommandAllocator* allocator = nullptr;
    ID3D12GraphicsCommandList7* list = nullptr;
    ID3D12DescriptorHeap* targets = nullptr;

    explicit HostD3d12(PrGpuDevice* gpu) {
        ID3D12Device* base = static_cast<ID3D12Device*>(prGpuD3d12Device(gpu));
        REQUIRE(SUCCEEDED(base->QueryInterface(IID_PPV_ARGS(&device))));
        REQUIRE(SUCCEEDED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))));
        REQUIRE(SUCCEEDED(device->CreateCommandList1(0, D3D12_COMMAND_LIST_TYPE_DIRECT, D3D12_COMMAND_LIST_FLAG_NONE,
            IID_PPV_ARGS(&list))));
        REQUIRE(SUCCEEDED(list->Reset(allocator, nullptr)));
        D3D12_DESCRIPTOR_HEAP_DESC heap{.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV, .NumDescriptors = 1};
        REQUIRE(SUCCEEDED(device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&targets))));
    }

    ~HostD3d12() {
        targets->Release();
        list->Release();
        allocator->Release();
        device->Release();
    }

    // Returns a render target view of a texture, in the host's heap.
    D3D12_CPU_DESCRIPTOR_HANDLE view(ID3D12Resource* texture) {
        D3D12_CPU_DESCRIPTOR_HANDLE handle = targets->GetCPUDescriptorHandleForHeapStart();
        device->CreateRenderTargetView(texture, nullptr, handle);
        return handle;
    }
};

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("d3d12 records into the host's command list, around the host's own work, and submits it") {
    Fixture f;
    if (f.device == nullptr)
        return;
    HostD3d12 host(f.device);

    // An upload made before the submission is visible to all of its work, the host's native command list's too.
    PrGpuBuffer output = f.buffer(16, PrGpuBufferUsageFlag_Storage);
    PrGpuPipeline pipeline{};
    REQUIRE(prGpuPipelineCreateCompute(f.device, f.library, "fill", "fill", &pipeline) == PrStatus_Ok);
    PrGpuCommandsOptions options = prGpuCommandsOptionsDefault();
    options.native = host.list;
    PrGpuCommands* commands = nullptr;
    REQUIRE(prGpuCommandsBegin(f.device, &options, &commands));
    uint32_t seed[4] = {9, 9, 9, 9};
    prGpuBufferUpload(f.device, output, 0, seed, sizeof(seed));
    Push push{.output = prGpuBufferStorageHandle(f.device, output), .count = 2, .scale = 11};
    prGpuCommandsSetPipeline(commands, pipeline);
    prGpuCommandsSetPush(commands, &push, sizeof(push));
    prGpuCommandsDispatch(commands, PrGpuExtent{1, 1, 1});
    PrGpuHostStateFlags changed = prGpuHostStateSync(commands);
    CHECK((changed & PrGpuHostStateFlag_Bindings) != 0);
    CHECK((changed & PrGpuHostStateFlag_Pipeline) != 0);
    CHECK((changed & PrGpuHostStateFlag_Push) != 0);
    REQUIRE(SUCCEEDED(host.list->Close()));
    PrGpuFence fence = prGpuCommandsSubmit(commands);
    prGpuDeviceWait(f.device, fence);
    CHECK(prGpuDeviceIsComplete(f.device, fence));
    CHECK(f.words(output, 4) == std::vector<uint32_t>{0, 11, 9, 9});
    prGpuPipelineDestroy(f.device, pipeline);
    prGpuBufferDestroy(f.device, output);
}

TEST_CASE("d3d12 draws into the host's pass, and says which of the pass's state its draws changed") {
    Fixture f;
    if (f.device == nullptr)
        return;
    HostD3d12 host(f.device);
    PrGpuTexture target = f.target();
    PrGpuPipeline pipeline = f.upperHalf();
    D3D12_CPU_DESCRIPTOR_HANDLE view = host.view(static_cast<ID3D12Resource*>(prGpuD3d12Texture(f.device, target)));

    PrGpuCommandsOptions options = prGpuCommandsOptionsDefault();
    options.native = host.list;
    PrGpuCommands* commands = nullptr;
    REQUIRE(prGpuCommandsBegin(f.device, &options, &commands));
    prGpuCommandsTextureBarrier(commands, target, PrGpuSubresources{}, PrGpuAccessFlag_Discard,
        PrGpuAccessFlag_ColorAttachment);
    prGpuHostStateSync(commands);

    // The host sets its render target and clears it to blue, and the device draws the upper half red into it.
    const float blue[4] = {0, 0, 1, 1};
    host.list->OMSetRenderTargets(1, &view, FALSE, nullptr);
    host.list->ClearRenderTargetView(view, blue, 0, nullptr);
    D3D12_VIEWPORT viewport{.Width = 8, .Height = 8, .MaxDepth = 1};
    D3D12_RECT scissor{.right = 8, .bottom = 8};
    host.list->RSSetViewports(1, &viewport);
    host.list->RSSetScissorRects(1, &scissor);
    PrGpuPassDesc passDesc{};
    passDesc.host = host.list;
    passDesc.hostFormats.colorCount = 1;
    passDesc.hostFormats.color[0] = PrGpuFormat_Rgba8;
    passDesc.hostFormats.samples = 1;
    passDesc.hostExtent = PrUVec2{8, 8};
    PrGpuPass* pass = prGpuPassBegin(commands, &passDesc);
    Uniforms uniforms{.color = {1, 0, 0, 1}};
    prGpuCommandsSetPipeline(commands, pipeline);
    prGpuCommandsSetUniforms(commands, &uniforms, sizeof(uniforms));
    prGpuPassDraw(pass, 4, 1, 0, 0);
    PrGpuHostStateFlags changed = prGpuHostStateSync(commands);
    CHECK((changed & PrGpuHostStateFlag_Pipeline) != 0);
    CHECK((changed & PrGpuHostStateFlag_Bindings) != 0);
    CHECK((changed & PrGpuHostStateFlag_IndexBuffer) == 0);
    CHECK(prGpuHostStateSync(commands) == 0);
    prGpuPassEnd(pass);

    prGpuCommandsTextureBarrier(commands, target, PrGpuSubresources{}, PrGpuAccessFlag_ColorAttachment,
        PrGpuAccessFlag_CopyRead);
    prGpuHostStateSync(commands);
    REQUIRE(SUCCEEDED(host.list->Close()));
    prGpuDeviceWait(f.device, prGpuCommandsSubmit(commands));
    uint32_t pixels[8 * 8]{};
    prGpuTextureRead(f.device, target, PrGpuAccessFlag_CopyRead, PrIVec2{0, 0},
        PrPixels{reinterpret_cast<uint8_t*>(pixels), 8 * 4, PrUVec2{8, 8}});
    CHECK(pixels[0] == 0xff0000ffu);
    CHECK(pixels[7 * 8 + 7] == 0xffff0000u);
    prGpuPipelineDestroy(f.device, pipeline);
    prGpuTextureDestroy(f.device, target);
}
