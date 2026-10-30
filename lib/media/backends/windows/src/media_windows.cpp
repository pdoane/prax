// The Media Foundation media codec.

#include "pr/media_windows.h"

#include "pr/alloc.h"
#include "pr/array.h"
#include "pr/ascii.h"
#include "pr/float.h"
#include "pr/memory.h"
#include "pr/mime.h"
#include "pr/mutex.h"
#include "pr/thread.h"

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <objbase.h>
#include <shlwapi.h>

namespace pr_impl {

namespace {

// The frames that a decoder keeps decoded ahead of the time last asked for.
constexpr uint32_t kFramesAhead = 4;

// How far past the last decoded frame a request may be before the decoder seeks to it rather than decoding toward it,
// in seconds.
constexpr double kSeekAhead = 1;

// The units of a Media Foundation time, per second.
constexpr double kTicksPerSecond = 10000000;

struct WindowsCodec : PrMediaCodec {
    // Keeps the multithreaded apartment alive, so that every thread that has not joined an apartment is implicitly a
    // member of it. A caller's thread and a decoding thread can then use Media Foundation without initializing COM.
    CO_MTA_USAGE_COOKIE apartment;
};

// How the rows of a decoded sample lie in its buffer.
struct SampleLayout {
    PrUVec2 frameExtent;    // the extent of the buffer, past the area that displays
    PrUVec2 origin;         // the top left of the area that displays
    PrUVec2 extent;         // the extent of the area that displays
    LONG stride;            // bytes from a row to the row below it, negative if the rows are stored bottom up
};

// A decoded frame, whose buffer holds tightly packed rows and which holds a reference to the buffer.
struct WindowsFrame {
    IMFMediaBuffer* buffer;
    PrUVec2 extent;
    double time;
    uint64_t serial;
};

struct WindowsDecoder : PrMediaDecoder {
    IMFSourceReader* reader;
    DWORD videoStream;
    bool hasVideo;

    // The decoding thread's alone once it starts.
    SampleLayout layout;

    // The decoding thread, which `mutex` and `wake` coordinate with the decoder's user.
    PrThread thread;
    PrMutex mutex;
    PrCondition wake;

    // Guarded by `mutex`.
    Array<WindowsFrame> frames; // decoded, in time order, from the last at or before `wanted` on
    double wanted;              // the time last asked for
    double seekTo;              // a time to restart decoding at, or NaN
    double start;               // the time that decoding last started at
    double through;             // the time of the last frame decoded since then, or -infinity
    bool dropped;               // frames decoded since `start` were dropped from the front
    bool ended;                 // every frame since `start` is decoded
    bool stopping;
    uint64_t serials;

    // The user's alone: the buffer of the frame last returned, retained and locked.
    IMFMediaBuffer* shown;
};

// A decoder that a codec of a MIME type's codecs parameter (rfc6381) needs.
struct CodecProbe {
    const char* prefix;
    const GUID* major;
    const GUID* subtype;
};

constexpr CodecProbe kCodecProbes[] = {
    {"avc1", &MFMediaType_Video, &MFVideoFormat_H264},
    {"avc3", &MFMediaType_Video, &MFVideoFormat_H264},
    {"hev1", &MFMediaType_Video, &MFVideoFormat_HEVC},
    {"hvc1", &MFMediaType_Video, &MFVideoFormat_HEVC},
    {"vp8", &MFMediaType_Video, &MFVideoFormat_VP80},
    {"vp9", &MFMediaType_Video, &MFVideoFormat_VP90},
    {"vp09", &MFMediaType_Video, &MFVideoFormat_VP90},
    {"av01", &MFMediaType_Video, &MFVideoFormat_AV1},
    {"mp4a", &MFMediaType_Audio, &MFAudioFormat_AAC},
    {"mp3", &MFMediaType_Audio, &MFAudioFormat_MP3},
    {"opus", &MFMediaType_Audio, &MFAudioFormat_Opus},
    {"flac", &MFMediaType_Audio, &MFAudioFormat_FLAC},
};

// The essences of the MIME types of the containers that Media Foundation reads.
constexpr PrStringView kContainerTypes[] = {
    "video/mp4", "audio/mp4", "video/quicktime", "audio/mpeg", "audio/mp3", "audio/wav", "audio/wave", "audio/x-wav",
    "video/webm", "audio/webm",
};

// Returns the MIME type of the container that the bytes begin with, or null for none that Media Foundation reads.
const wchar_t* PR_NULLABLE containerType(PrByteView bytes) {
    const uint8_t* b = bytes.data;
    if (bytes.size >= 12 && memcmp(b + 4, "ftyp", 4) == 0)
        return memcmp(b + 8, "qt  ", 4) == 0 ? L"video/quicktime" : L"video/mp4";
    if (bytes.size >= 8 && (memcmp(b + 4, "moov", 4) == 0 || memcmp(b + 4, "mdat", 4) == 0))
        return L"video/quicktime";
    if (bytes.size >= 12 && memcmp(b, "RIFF", 4) == 0 && memcmp(b + 8, "WAVE", 4) == 0)
        return L"audio/wav";
    if (bytes.size >= 3 && (memcmp(b, "ID3", 3) == 0 || (b[0] == 0xff && (b[1] & 0xe0) == 0xe0)))
        return L"audio/mpeg";
    if (bytes.size >= 4 && memcmp(b, "\x1a\x45\xdf\xa3", 4) == 0)
        return L"video/webm";
    return nullptr;
}

// Returns the status of a failure to open a resource: the system lacks what the resource needs, or the resource is
// corrupt.
PrStatus statusFromResult(HRESULT result) {
    switch (result) {
        case MF_E_UNSUPPORTED_BYTESTREAM_TYPE:
        case MF_E_BYTESTREAM_NOT_SEEKABLE:
        case MF_E_TOPO_CODEC_NOT_FOUND:
        case MF_E_INVALIDMEDIATYPE:
            return PrStatus_NotSupported;
        default:
            return PrStatus_InvalidData;
    }
}

// Reads the area of a video type that displays into `origin` and `extent`, inside the frame that `frameExtent` gives.
void readDisplayArea(IMFMediaType* type, PrUVec2 frameExtent, PrUVec2* origin, PrUVec2* extent) {
    *origin = PrUVec2{};
    *extent = frameExtent;
    MFVideoArea area{};
    if (FAILED(type->GetBlob(MF_MT_MINIMUM_DISPLAY_APERTURE, reinterpret_cast<UINT8*>(&area), sizeof(area), nullptr)))
        return;
    uint32_t x = uint32_t(area.OffsetX.value);
    uint32_t y = uint32_t(area.OffsetY.value);
    if (area.Area.cx <= 0 || area.Area.cy <= 0 || x + uint32_t(area.Area.cx) > frameExtent.x ||
            y + uint32_t(area.Area.cy) > frameExtent.y) {
        return;
    }
    *origin = PrUVec2{x, y};
    *extent = PrUVec2{uint32_t(area.Area.cx), uint32_t(area.Area.cy)};
}

// Returns the extent of a track of the native video `type` as it displays: its area that displays, stretched to square
// pixels and turned by its rotation.
PrUVec2 displayExtent(IMFMediaType* type) {
    UINT32 width = 0;
    UINT32 height = 0;
    if (FAILED(MFGetAttributeSize(type, MF_MT_FRAME_SIZE, &width, &height)))
        return PrUVec2{};
    PrUVec2 origin{};
    PrUVec2 extent{};
    readDisplayArea(type, PrUVec2{width, height}, &origin, &extent);
    UINT32 numerator = 1;
    UINT32 denominator = 1;
    bool hasRatio = SUCCEEDED(MFGetAttributeRatio(type, MF_MT_PIXEL_ASPECT_RATIO, &numerator, &denominator));
    if (hasRatio && numerator != 0 && denominator != 0)
        extent.x = uint32_t((uint64_t(extent.x) * numerator + denominator / 2) / denominator);
    UINT32 rotation = MFGetAttributeUINT32(type, MF_MT_VIDEO_ROTATION, MFVideoRotationFormat_0);
    if (rotation == MFVideoRotationFormat_90 || rotation == MFVideoRotationFormat_270)
        return PrUVec2{extent.y, extent.x};
    return extent;
}

// Reads how the samples that the reader now gives lie in their buffers. Returns false if the reader does not say.
bool readLayout(IMFSourceReader* reader, DWORD stream, SampleLayout* out) {
    IMFMediaType* type = nullptr;
    if (FAILED(reader->GetCurrentMediaType(stream, &type)))
        return false;
    UINT32 width = 0;
    UINT32 height = 0;
    LONG stride = 0;
    bool ok = SUCCEEDED(MFGetAttributeSize(type, MF_MT_FRAME_SIZE, &width, &height));
    if (ok) {
        out->frameExtent = PrUVec2{width, height};
        readDisplayArea(type, out->frameExtent, &out->origin, &out->extent);
        ok = SUCCEEDED(MFGetStrideForBitmapInfoHeader(MFVideoFormat_RGB32.Data1, width, &stride));
    }
    if (ok)
        out->stride = LONG(MFGetAttributeUINT32(type, MF_MT_DEFAULT_STRIDE, UINT32(stride)));
    type->Release();
    return ok;
}

// Copies the area that displays of `sample` to a new buffer of tightly packed opaque rows, top down, into `frame`.
// Returns false if the sample's buffer is smaller than its layout says.
bool copySample(IMFSample* sample, const SampleLayout& layout, WindowsFrame* frame) {
    IMFMediaBuffer* source = nullptr;
    if (FAILED(sample->ConvertToContiguousBuffer(&source)))
        return false;
    IMF2DBuffer* surface = nullptr;
    BYTE* first = nullptr;
    LONG stride = layout.stride;
    bool locked = false;
    if (SUCCEEDED(source->QueryInterface(IID_PPV_ARGS(&surface))))
        locked = SUCCEEDED(surface->Lock2D(&first, &stride));
    bool ok = locked;
    if (!locked) {
        BYTE* data = nullptr;
        DWORD length = 0;
        ok = SUCCEEDED(source->Lock(&data, nullptr, &length));
        uint64_t rowSize = uint64_t(stride < 0 ? -int64_t(stride) : int64_t(stride));
        if (ok && length < rowSize * layout.frameExtent.y) {
            source->Unlock();
            ok = false;
        }
        if (ok)
            first = stride < 0 ? data + rowSize * (layout.frameExtent.y - 1) : data;
    }
    IMFMediaBuffer* buffer = nullptr;
    size_t rowBytes = size_t(layout.extent.x) * 4;
    DWORD size = DWORD(rowBytes * layout.extent.y);
    BYTE* target = nullptr;
    if (ok && (FAILED(MFCreateMemoryBuffer(size, &buffer)) || FAILED(buffer->Lock(&target, nullptr, nullptr)))) {
        if (buffer != nullptr)
            buffer->Release();
        buffer = nullptr;
    }
    if (buffer != nullptr) {
        for (uint32_t y = 0; y < layout.extent.y; ++y) {
            const BYTE* row = first + (ptrdiff_t(layout.origin.y) + y) * stride + size_t(layout.origin.x) * 4;
            BYTE* to = target + y * rowBytes;
            memcpy(to, row, rowBytes);
            // The system's RGB32 leaves the fourth byte of a pixel undefined.
            for (uint32_t x = 0; x < layout.extent.x; ++x)
                to[x * 4 + 3] = 255;
        }
        buffer->Unlock();
        buffer->SetCurrentLength(size);
        frame->buffer = buffer;
        frame->extent = layout.extent;
    }
    if (locked)
        surface->Unlock2D();
    else if (ok)
        source->Unlock();
    if (surface != nullptr)
        surface->Release();
    source->Release();
    return buffer != nullptr;
}

// Releases the frames, and empties the list.
void releaseFrames(Array<WindowsFrame>* frames) {
    for (uint32_t i = 0; i < frames->count; ++i)
        frames->data[i].buffer->Release();
    frames->clear();
}

// Moves the reader to the key frame at or before `seconds`. Returns false if the reader cannot move.
bool seekReader(IMFSourceReader* reader, double seconds) {
    PROPVARIANT position{};
    position.vt = VT_I8;
    position.hVal.QuadPart = LONGLONG(seconds * kTicksPerSecond);
    return SUCCEEDED(reader->SetCurrentPosition(GUID_NULL, position));
}

// Decodes frames while fewer than kFramesAhead wait past the time last asked for, and restarts at each seek, until the
// decoder stops. A seek lands on a key frame, so the frames before the time that it asked for are decoded and dropped.
void decodeFrames(WindowsDecoder* d) {
    prMutexLock(&d->mutex);
    while (!d->stopping) {
        if (!isNanF64(d->seekTo)) {
            double at = d->seekTo;
            d->seekTo = kNanF64;
            releaseFrames(&d->frames);
            d->start = at;
            d->through = -kInfinityF64;
            d->dropped = false;
            d->ended = false;
            prMutexUnlock(&d->mutex);
            bool moved = seekReader(d->reader, at);
            prMutexLock(&d->mutex);
            if (!moved)
                d->ended = true;
            continue;
        }
        uint32_t ahead = 0;
        for (uint32_t i = 0; i < d->frames.count; ++i)
            ahead += d->frames.data[i].time > d->wanted ? 1 : 0;
        if (d->ended || ahead >= kFramesAhead) {
            prConditionWait(&d->wake, &d->mutex);
            continue;
        }
        prMutexUnlock(&d->mutex);
        DWORD flags = 0;
        LONGLONG ticks = 0;
        IMFSample* sample = nullptr;
        HRESULT result = d->reader->ReadSample(d->videoStream, 0, nullptr, &flags, &ticks, &sample);
        bool finished = FAILED(result) || (flags & MF_SOURCE_READERF_ENDOFSTREAM) != 0;
        if (SUCCEEDED(result) && (flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) != 0)
            finished = !readLayout(d->reader, d->videoStream, &d->layout);
        WindowsFrame frame{};
        bool copied = sample != nullptr && !finished && copySample(sample, d->layout, &frame);
        if (sample != nullptr)
            sample->Release();
        prMutexLock(&d->mutex);
        if (!isNanF64(d->seekTo)) {
            if (copied)
                frame.buffer->Release();
            continue;
        }
        if (finished) {
            d->ended = true;
            continue;
        }
        if (!copied)
            continue;
        frame.time = double(ticks) / kTicksPerSecond;
        frame.serial = ++d->serials;
        d->frames.push(frame);
        d->through = frame.time;
        // Only the last frame at or before the time last asked for can show until the time moves. Dropping the others
        // bounds what a long run from a key frame to a seek's time holds.
        while (d->frames.count >= 2 && d->frames.data[1].time <= d->wanted) {
            d->frames.data[0].buffer->Release();
            d->frames.erase(0, 1);
            d->dropped = true;
        }
    }
    prMutexUnlock(&d->mutex);
}

// Returns true if the system has a decoder of what a part of a codecs parameter names.
bool hasDecoder(PrStringView codec) {
    for (const CodecProbe& probe : kCodecProbes) {
        if (!asciiStartsWithFold(codec, probe.prefix))
            continue;
        MFT_REGISTER_TYPE_INFO input{*probe.major, *probe.subtype};
        GUID category = *probe.major == MFMediaType_Video ? MFT_CATEGORY_VIDEO_DECODER : MFT_CATEGORY_AUDIO_DECODER;
        IMFActivate** found = nullptr;
        UINT32 count = 0;
        if (FAILED(MFTEnumEx(category, MFT_ENUM_FLAG_ALL, &input, nullptr, &found, &count)))
            return false;
        for (UINT32 i = 0; i < count; ++i)
            found[i]->Release();
        CoTaskMemFree(found);
        return count != 0;
    }
    return false;
}

PrMediaSupport supportsType(PrMediaCodec*, PrStringView type) {
    PrStringView essence = prMimeTypeEssence(type);
    bool known = false;
    for (PrStringView container : kContainerTypes)
        known |= asciiEqualsFold(essence, container);
    if (!known)
        return PrMediaSupport_No;
    PrString parameter{};
    if (!prMimeTypeParameter(type, "codecs", &parameter))
        return PrMediaSupport_Maybe;
    PrStringView codecs = prStringView(&parameter);
    // A type that names its codecs, which the system decodes, is probably playable.
    PrMediaSupport support = PrMediaSupport_Probably;
    size_t from = 0;
    while (from <= codecs.size && support != PrMediaSupport_No) {
        size_t end = from;
        while (end < codecs.size && codecs.data[end] != ',')
            ++end;
        size_t begin = from;
        while (begin < end && asciiIsHttpSpace(codecs.data[begin]))
            ++begin;
        if (!hasDecoder(PrStringView{codecs.data + begin, end - begin}))
            support = PrMediaSupport_No;
        from = end + 1;
    }
    prStringReset(&parameter);
    return support;
}

// Creates a reader of the resource in `bytes`, which the reader's stream copies, and which is of the MIME type
// `container`. The reader converts video to RGB32.
HRESULT readerCreate(PrByteView bytes, const wchar_t* container, IMFSourceReader** out) {
    IStream* memory = SHCreateMemStream(bytes.data, UINT(bytes.size));
    if (memory == nullptr)
        return E_OUTOFMEMORY;
    IMFByteStream* stream = nullptr;
    IMFAttributes* streamAttributes = nullptr;
    IMFAttributes* attributes = nullptr;
    HRESULT result = MFCreateMFByteStreamOnStream(memory, &stream);
    if (SUCCEEDED(result))
        result = stream->QueryInterface(IID_PPV_ARGS(&streamAttributes));
    if (SUCCEEDED(result))
        result = streamAttributes->SetString(MF_BYTESTREAM_CONTENT_TYPE, container);
    if (SUCCEEDED(result))
        result = MFCreateAttributes(&attributes, 1);
    if (SUCCEEDED(result))
        result = attributes->SetUINT32(MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, TRUE);
    if (SUCCEEDED(result))
        result = MFCreateSourceReaderFromByteStream(stream, attributes, out);
    if (attributes != nullptr)
        attributes->Release();
    if (streamAttributes != nullptr)
        streamAttributes->Release();
    if (stream != nullptr)
        stream->Release();
    memory->Release();
    return result;
}

// Writes what the reader's streams hold to `info`, and the first video stream to `videoStream`.
void readStreams(IMFSourceReader* reader, PrMediaInfo* info, DWORD* videoStream) {
    for (DWORD stream = 0;; ++stream) {
        IMFMediaType* type = nullptr;
        if (FAILED(reader->GetNativeMediaType(stream, 0, &type)))
            return;
        GUID major{};
        if (SUCCEEDED(type->GetGUID(MF_MT_MAJOR_TYPE, &major))) {
            if (major == MFMediaType_Audio)
                info->hasAudio = true;
            if (major == MFMediaType_Video && !info->hasVideo) {
                info->hasVideo = true;
                info->videoExtent = displayExtent(type);
                *videoStream = stream;
            }
        }
        type->Release();
    }
}

// Selects only the video stream, and asks the reader for it as RGB32.
HRESULT selectVideo(IMFSourceReader* reader, DWORD stream) {
    HRESULT result = reader->SetStreamSelection(DWORD(MF_SOURCE_READER_ALL_STREAMS), FALSE);
    if (SUCCEEDED(result))
        result = reader->SetStreamSelection(stream, TRUE);
    IMFMediaType* type = nullptr;
    if (SUCCEEDED(result))
        result = MFCreateMediaType(&type);
    if (SUCCEEDED(result))
        result = type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    if (SUCCEEDED(result))
        result = type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
    if (SUCCEEDED(result))
        result = reader->SetCurrentMediaType(stream, nullptr, type);
    if (type != nullptr)
        type->Release();
    return result;
}

PrStatus decoderCreate(PrMediaCodec*, PrByteView bytes, PrMediaDecoder* PR_NULLABLE* PR_NONNULL out, PrMediaInfo* info) {
    const wchar_t* container = containerType(bytes);
    if (container == nullptr || bytes.size > UINT32_MAX)
        return PrStatus_NotSupported;
    IMFSourceReader* reader = nullptr;
    HRESULT result = readerCreate(bytes, container, &reader);
    if (FAILED(result))
        return statusFromResult(result);
    DWORD videoStream = 0;
    readStreams(reader, info, &videoStream);
    if (!info->hasVideo && !info->hasAudio) {
        reader->Release();
        return PrStatus_InvalidData;
    }
    SampleLayout layout{};
    if (info->hasVideo) {
        result = selectVideo(reader, videoStream);
        if (FAILED(result)) {
            reader->Release();
            return statusFromResult(result);
        }
        if (!readLayout(reader, videoStream, &layout)) {
            reader->Release();
            return PrStatus_InvalidData;
        }
    }
    info->duration = kNanF64;
    PROPVARIANT duration{};
    if (SUCCEEDED(reader->GetPresentationAttribute(DWORD(MF_SOURCE_READER_MEDIASOURCE), MF_PD_DURATION, &duration))) {
        if (duration.vt == VT_UI8)
            info->duration = double(duration.uhVal.QuadPart) / kTicksPerSecond;
        PropVariantClear(&duration);
    }
    WindowsDecoder* d = allocZeroed<WindowsDecoder>();
    d->reader = reader;
    d->videoStream = videoStream;
    d->hasVideo = info->hasVideo;
    d->layout = layout;
    d->seekTo = kNanF64;
    d->through = -kInfinityF64;
    d->ended = !info->hasVideo;
    prMutexInit(&d->mutex);
    prConditionInit(&d->wake);
    if (info->hasVideo) {
        d->seekTo = 0;
        prThreadStart(&d->thread, [](void* context) { decodeFrames(static_cast<WindowsDecoder*>(context)); }, d);
    }
    *out = d;
    return PrStatus_Ok;
}

void decoderDestroy(PrMediaCodec*, PrMediaDecoder* decoder) {
    WindowsDecoder* d = static_cast<WindowsDecoder*>(decoder);
    if (d->hasVideo) {
        prMutexLock(&d->mutex);
        d->stopping = true;
        prConditionSignal(&d->wake);
        prMutexUnlock(&d->mutex);
        prThreadJoin(&d->thread);
    }
    if (d->shown != nullptr) {
        d->shown->Unlock();
        d->shown->Release();
    }
    releaseFrames(&d->frames);
    d->frames.reset();
    prConditionReset(&d->wake);
    prMutexReset(&d->mutex);
    d->reader->Release();
    prMemoryFree(d);
}

bool decoderFrame(PrMediaCodec*, PrMediaDecoder* decoder, double seconds, PrMediaFrame* out) {
    WindowsDecoder* d = static_cast<WindowsDecoder*>(decoder);
    prMutexLock(&d->mutex);
    d->wanted = seconds;
    // A time before what decoding kept, or far past what it reached, restarts decoding there.
    bool before = seconds < d->start || (d->dropped && d->frames.count != 0 && seconds < d->frames.data[0].time);
    bool past = !d->ended && seconds > d->through + kSeekAhead && seconds > d->start + kSeekAhead;
    if (isNanF64(d->seekTo) && (before || past))
        d->seekTo = seconds;
    // The frame that shows is the last that starts at or before the time. Before the first frame, the first shows.
    uint32_t at = UINT32_MAX;
    if (isNanF64(d->seekTo)) {
        for (uint32_t i = 0; i < d->frames.count && d->frames.data[i].time <= seconds; ++i)
            at = i;
        if (at == UINT32_MAX && d->frames.count != 0 && !d->dropped)
            at = 0;
    }
    WindowsFrame frame{};
    bool current = false;
    if (at != UINT32_MAX) {
        // The frame shows at the time if the frame after it is decoded, or none follows, or it is the first frame and
        // shows before its own time.
        current = at + 1 < d->frames.count || d->ended || d->frames.data[at].time > seconds;
        for (uint32_t i = 0; i < at; ++i)
            d->frames.data[i].buffer->Release();
        d->frames.erase(0, at);
        d->dropped |= at != 0;
        frame = d->frames.data[0];
        frame.buffer->AddRef();
    }
    // Without a frame, none will come once decoding has ended with nothing kept.
    bool done = frame.buffer == nullptr && isNanF64(d->seekTo) && d->ended && d->frames.count == 0;
    prConditionSignal(&d->wake);
    prMutexUnlock(&d->mutex);
    if (frame.buffer == nullptr) {
        out->current = done;
        return false;
    }
    if (d->shown != nullptr) {
        d->shown->Unlock();
        d->shown->Release();
    }
    d->shown = frame.buffer;
    BYTE* pixels = nullptr;
    if (FAILED(frame.buffer->Lock(&pixels, nullptr, nullptr))) {
        frame.buffer->Release();
        d->shown = nullptr;
        return false;
    }
    *out = PrMediaFrame{
        .pixels = PrPixelView{pixels, size_t(frame.extent.x) * 4, frame.extent},
        .time = frame.time,
        .serial = frame.serial,
        .current = current,
    };
    return true;
}

void destroy(PrMediaCodec* codec) {
    WindowsCodec* windows = static_cast<WindowsCodec*>(codec);
    MFShutdown();
    CoDecrementMTAUsage(windows->apartment);
    prMemoryFree(windows);
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

PrMediaCodec* PR_NULLABLE prMediaWindowsCreate(void) {
    CO_MTA_USAGE_COOKIE apartment = nullptr;
    if (FAILED(CoIncrementMTAUsage(&apartment)))
        return nullptr;
    if (FAILED(MFStartup(MF_VERSION))) {
        CoDecrementMTAUsage(apartment);
        return nullptr;
    }
    WindowsCodec* codec = allocZeroed<WindowsCodec>();
    codec->destroy = destroy;
    codec->supportsType = supportsType;
    codec->decoderCreate = decoderCreate;
    codec->decoderDestroy = decoderDestroy;
    codec->decoderFrame = decoderFrame;
    codec->apartment = apartment;
    return codec;
}
