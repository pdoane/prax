// The Core Audio output.

#include "pr/audio_apple.h"

#include "pr/alloc.h"
#include "pr/audio_backend.h"
#include "pr/memory.h"

#include <AudioToolbox/AudioToolbox.h>
#include <TargetConditionals.h>
#if TARGET_OS_OSX
#include <CoreAudio/CoreAudio.h>
#endif

namespace pr_impl {

namespace {

struct AppleOutput : PrAudioOutput {
    PrAudioFormat format;
    AudioUnit unit;
    PrAudioRenderFn render;
    void* context;
    bool running;
};

OSStatus renderCallback(void* context, AudioUnitRenderActionFlags*, const AudioTimeStamp*, UInt32, UInt32 frames,
        AudioBufferList* buffers) {
    AppleOutput* output = static_cast<AppleOutput*>(context);
    output->render(output->context, static_cast<float*>(buffers->mBuffers[0].mData), frames);
    return noErr;
}

bool start(PrAudioOutput* base, PrAudioRenderFn render, void* PR_NULLABLE context) {
    AppleOutput* output = static_cast<AppleOutput*>(base);
    output->render = render;
    output->context = context;
    output->running = AudioOutputUnitStart(output->unit) == noErr;
    return output->running;
}

void stop(PrAudioOutput* base) {
    AppleOutput* output = static_cast<AppleOutput*>(base);
    if (output->running)
        AudioOutputUnitStop(output->unit);
    output->running = false;
}

double latency(PrAudioOutput* base) {
    AppleOutput* output = static_cast<AppleOutput*>(base);
    double seconds = 0;
    Float64 unitLatency = 0;
    UInt32 size = sizeof(unitLatency);
    if (AudioUnitGetProperty(output->unit, kAudioUnitProperty_Latency, kAudioUnitScope_Global, 0, &unitLatency, &size) == noErr)
        seconds += unitLatency;
#if TARGET_OS_OSX
    // The device's own latency, its safety offset and its buffer, in frames at the device's rate.
    AudioDeviceID device = 0;
    size = sizeof(device);
    if (AudioUnitGetProperty(output->unit, kAudioOutputUnitProperty_CurrentDevice, kAudioUnitScope_Global, 0, &device,
            &size) != noErr)
        return seconds;
    Float64 rate = 0;
    AudioObjectPropertyAddress address{
        kAudioDevicePropertyNominalSampleRate, kAudioObjectPropertyScopeOutput,
        kAudioObjectPropertyElementMain,
    };
    size = sizeof(rate);
    if (AudioObjectGetPropertyData(device, &address, 0, nullptr, &size, &rate) != noErr || rate == 0)
        return seconds;
    constexpr AudioObjectPropertySelector kFrameCounts[] = {
        kAudioDevicePropertyLatency,
        kAudioDevicePropertySafetyOffset,
        kAudioDevicePropertyBufferFrameSize,
    };
    for (AudioObjectPropertySelector selector : kFrameCounts) {
        UInt32 frames = 0;
        address.mSelector = selector;
        size = sizeof(frames);
        if (AudioObjectGetPropertyData(device, &address, 0, nullptr, &size, &frames) == noErr)
            seconds += double(frames) / rate;
    }
#endif
    return seconds;
}

void destroy(PrAudioOutput* base) {
    AppleOutput* output = static_cast<AppleOutput*>(base);
    stop(output);
    AudioUnitUninitialize(output->unit);
    AudioComponentInstanceDispose(output->unit);
    prMemoryFree(output);
}

PrAudioFormat formatOf(const PrAudioOutput* output) {
    return static_cast<const AppleOutput*>(output)->format;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

PrAudioOutput* PR_NULLABLE prAudioAppleCreate(PrAudioFormat format) {
    AudioComponentDescription description{};
    description.componentType = kAudioUnitType_Output;
#if TARGET_OS_OSX
    description.componentSubType = kAudioUnitSubType_DefaultOutput;
#else
    description.componentSubType = kAudioUnitSubType_RemoteIO;
#endif
    description.componentManufacturer = kAudioUnitManufacturer_Apple;
    AudioComponent component = AudioComponentFindNext(nullptr, &description);
    AudioUnit unit = nullptr;
    if (component == nullptr || AudioComponentInstanceNew(component, &unit) != noErr)
        return nullptr;
    AppleOutput* output = allocZeroed<AppleOutput>();
    output->format = format;
    output->destroyFn = destroy;
    output->formatFn = formatOf;
    output->startFn = start;
    output->stopFn = stop;
    output->latencyFn = latency;
    output->unit = unit;
    AudioStreamBasicDescription stream{};
    stream.mSampleRate = format.sampleRate;
    stream.mFormatID = kAudioFormatLinearPCM;
    stream.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
    stream.mBitsPerChannel = 32;
    stream.mChannelsPerFrame = format.channels;
    stream.mFramesPerPacket = 1;
    stream.mBytesPerFrame = format.channels * sizeof(float);
    stream.mBytesPerPacket = stream.mBytesPerFrame;
    AURenderCallbackStruct callback{renderCallback, output};
    bool ok = AudioUnitSetProperty(unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, &stream,
        sizeof(stream)) == noErr;
    ok = ok && AudioUnitSetProperty(unit, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0, &callback,
        sizeof(callback)) == noErr;
    ok = ok && AudioUnitInitialize(unit) == noErr;
    if (!ok) {
        AudioComponentInstanceDispose(unit);
        prMemoryFree(output);
        return nullptr;
    }
    return output;
}
