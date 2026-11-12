// The WASAPI output.

#include "pr/audio_windows.h"

#include "pr/alloc.h"
#include "pr/atomic.h"
#include "pr/audio_backend.h"
#include "pr/memory.h"
#include "pr/thread.h"

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <audioclient.h>
#include <avrt.h>
#include <mmdeviceapi.h>
#include <mmreg.h>
#include <objbase.h>

namespace pr_impl {

namespace {

// The buffer that the output asks the system for, in units of 100 nanoseconds.
constexpr REFERENCE_TIME kBufferTicks = 200000;

constexpr double kTicksPerSecond = 10000000;

struct WindowsOutput : PrAudioOutput {
    PrAudioFormat format;

    // Keeps the multithreaded apartment alive, so that the render thread can use the client without initializing COM.
    CO_MTA_USAGE_COOKIE apartment;
    IAudioClient* client;
    IAudioRenderClient* renderer;
    HANDLE ready;               // signaled when the device wants more frames
    UINT32 bufferFrames;
    PrThread thread;
    PrAudioRenderFn render;
    void* context;
    Atomic<bool> stopping;
    bool running;
};

void renderLoop(WindowsOutput* output) {
    DWORD task = 0;
    HANDLE priority = AvSetMmThreadCharacteristicsW(L"Pro Audio", &task);
    while (!output->stopping.load()) {
        if (WaitForSingleObject(output->ready, 200) != WAIT_OBJECT_0)
            continue;
        UINT32 padding = 0;
        if (FAILED(output->client->GetCurrentPadding(&padding)))
            continue;
        UINT32 frames = output->bufferFrames - padding;
        BYTE* samples = nullptr;
        if (frames == 0 || FAILED(output->renderer->GetBuffer(frames, &samples)))
            continue;
        output->render(output->context, reinterpret_cast<float*>(samples), frames);
        output->renderer->ReleaseBuffer(frames, 0);
    }
    if (priority != nullptr)
        AvRevertMmThreadCharacteristics(priority);
}

bool start(PrAudioOutput* base, PrAudioRenderFn render, void* PR_NULLABLE context) {
    WindowsOutput* output = static_cast<WindowsOutput*>(base);
    output->render = render;
    output->context = context;
    output->stopping.store(false);
    if (FAILED(output->client->Start()))
        return false;
    output->running = true;
    prThreadStart(&output->thread, [](void* context) { renderLoop(static_cast<WindowsOutput*>(context)); }, output);
    return true;
}

void stop(PrAudioOutput* base) {
    WindowsOutput* output = static_cast<WindowsOutput*>(base);
    if (!output->running)
        return;
    output->stopping.store(true);
    prThreadJoin(&output->thread);
    output->client->Stop();
    output->client->Reset();
    output->running = false;
}

double latency(PrAudioOutput* base) {
    WindowsOutput* output = static_cast<WindowsOutput*>(base);
    REFERENCE_TIME stream = 0;
    double seconds = 0;
    if (SUCCEEDED(output->client->GetStreamLatency(&stream)))
        seconds = double(stream) / kTicksPerSecond;
    return seconds + double(output->bufferFrames) / output->format.sampleRate;
}

void destroy(PrAudioOutput* base) {
    WindowsOutput* output = static_cast<WindowsOutput*>(base);
    stop(output);
    if (output->renderer != nullptr)
        output->renderer->Release();
    if (output->client != nullptr)
        output->client->Release();
    if (output->ready != nullptr)
        CloseHandle(output->ready);
    CoDecrementMTAUsage(output->apartment);
    prMemoryFree(output);
}

// Opens the default device's client in `output`. Returns false if the system has no device, or the device refuses.
bool openClient(WindowsOutput* output, PrAudioFormat format) {
    IMMDeviceEnumerator* enumerator = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator))))
        return false;
    IMMDevice* device = nullptr;
    HRESULT result = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
    enumerator->Release();
    if (FAILED(result))
        return false;
    result = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&output->client));
    device->Release();
    if (FAILED(result))
        return false;
    WAVEFORMATEX wave{};
    wave.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
    wave.nChannels = WORD(format.channels);
    wave.nSamplesPerSec = format.sampleRate;
    wave.wBitsPerSample = 32;
    wave.nBlockAlign = WORD(format.channels * sizeof(float));
    wave.nAvgBytesPerSec = format.sampleRate * wave.nBlockAlign;
    DWORD flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
        AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
    if (FAILED(output->client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, kBufferTicks, 0, &wave, nullptr)))
        return false;
    output->ready = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    return output->ready != nullptr && SUCCEEDED(output->client->SetEventHandle(output->ready)) &&
        SUCCEEDED(output->client->GetBufferSize(&output->bufferFrames)) &&
        SUCCEEDED(output->client->GetService(IID_PPV_ARGS(&output->renderer)));
}

PrAudioFormat formatOf(const PrAudioOutput* output) {
    return static_cast<const WindowsOutput*>(output)->format;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

PrAudioOutput* PR_NULLABLE prAudioWindowsCreate(PrAudioFormat format) {
    CO_MTA_USAGE_COOKIE apartment = nullptr;
    if (FAILED(CoIncrementMTAUsage(&apartment)))
        return nullptr;
    WindowsOutput* output = allocZeroed<WindowsOutput>();
    output->format = format;
    output->destroyFn = destroy;
    output->formatFn = formatOf;
    output->startFn = start;
    output->stopFn = stop;
    output->latencyFn = latency;
    output->apartment = apartment;
    if (!openClient(output, format)) {
        destroy(output);
        return nullptr;
    }
    return output;
}
