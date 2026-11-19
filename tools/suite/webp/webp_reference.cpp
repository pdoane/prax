#include "tools/suite/webp/webp_reference.h"

#include "pr/mem.h"
#include "pr/webp.h"

#include "webp/decode.h"
#include "webp/demux.h"
#include "webp/encode.h"
#include "webp/mux.h"

using namespace pr;

namespace webp {

namespace {

void copyPlane(const uint8_t* from, size_t stride, uint32_t width, uint32_t height, Array<uint8_t>* out) {
    out->resize(width * height);
    for (uint32_t y = 0; y < height; ++y)
        prMemCopy(out->data() + size_t(y) * width, from + y * stride, width);
}

void configure(const Options& options, WebPConfig* config) {
    WebPConfigInit(config);
    config->lossless = options.lossless ? 1 : 0;
    config->quality = options.quality;
    config->method = options.method;
    config->exact = options.exact ? 1 : 0;
    config->near_lossless = options.nearLossless;
    config->segments = options.segments;
    config->sns_strength = options.sns;
    config->filter_strength = options.filterStrength;
    config->filter_sharpness = options.filterSharpness;
    config->filter_type = options.filterType;
    config->autofilter = options.autofilter ? 1 : 0;
    config->partitions = options.partitions;
    config->alpha_compression = options.alphaCompression;
    config->alpha_filtering = options.alphaFiltering;
    config->alpha_quality = options.alphaQuality;
    config->use_sharp_yuv = options.sharpYuv ? 1 : 0;
}

bool importPicture(const uint8_t* rgba, uint32_t width, uint32_t height, bool lossless, WebPPicture* picture) {
    if (!WebPPictureInit(picture))
        return false;
    picture->width = int(width);
    picture->height = int(height);
    picture->use_argb = lossless ? 1 : 0;
    return WebPPictureImportRGBA(picture, rgba, int(width * 4)) != 0;
}

} // namespace

bool referenceFrames(PrByteView file, Frames* out) {
    *out = Frames{};
    WebPBitstreamFeatures features = {};
    if (WebPGetFeatures(file.data, file.size, &features) != VP8_STATUS_OK)
        return false;
    if (!features.has_animation) {
        int width = 0;
        int height = 0;
        uint8_t* rgba = WebPDecodeRGBA(file.data, file.size, &width, &height);
        if (rgba == nullptr)
            return false;
        out->width = uint32_t(width);
        out->height = uint32_t(height);
        out->playCount = 1;
        out->rgba.append(rgba, uint32_t(width * height * 4));
        out->durations.push(0);
        WebPFree(rgba);
        return true;
    }
    WebPAnimDecoderOptions options = {};
    if (!WebPAnimDecoderOptionsInit(&options))
        return false;
    options.color_mode = MODE_RGBA;
    options.use_threads = 0;
    const WebPData data = {file.data, file.size};
    WebPAnimDecoder* decoder = WebPAnimDecoderNew(&data, &options);
    if (decoder == nullptr)
        return false;
    WebPAnimInfo info = {};
    bool ok = WebPAnimDecoderGetInfo(decoder, &info) != 0;
    out->width = info.canvas_width;
    out->height = info.canvas_height;
    out->playCount = info.loop_count;
    int previous = 0;
    while (ok && WebPAnimDecoderHasMoreFrames(decoder)) {
        uint8_t* pixels = nullptr;
        int timestamp = 0;
        ok = WebPAnimDecoderGetNext(decoder, &pixels, &timestamp) != 0;
        if (ok) {
            out->rgba.append(pixels, out->width * out->height * 4);
            out->durations.push(uint32_t(timestamp - previous));
            previous = timestamp;
        }
    }
    WebPAnimDecoderDelete(decoder);
    return ok;
}

bool referencePlanes(PrByteView file, Planes* out) {
    *out = Planes{};
    int width = 0;
    int height = 0;
    uint8_t* u = nullptr;
    uint8_t* v = nullptr;
    int stride = 0;
    int uvStride = 0;
    uint8_t* y = WebPDecodeYUV(file.data, file.size, &width, &height, &u, &v, &stride, &uvStride);
    if (y == nullptr)
        return false;
    out->width = uint32_t(width);
    out->height = uint32_t(height);
    copyPlane(y, size_t(stride), out->width, out->height, &out->y);
    copyPlane(u, size_t(uvStride), (out->width + 1) / 2, (out->height + 1) / 2, &out->u);
    copyPlane(v, size_t(uvStride), (out->width + 1) / 2, (out->height + 1) / 2, &out->v);
    WebPFree(y);
    return true;
}

PrStatus praxFrames(PrByteView file, Frames* out) {
    *out = Frames{};
    PrWebpDecoder* decoder = nullptr;
    PrStatus status = prWebpDecoderCreate(file, nullptr, file.size, &decoder);
    if (status != PrStatus_Ok) {
        prWebpDecoderDestroy(decoder);
        return status;
    }
    const PrWebpInfo& info = *prWebpDecoderInfo(decoder);
    out->width = info.extent.x;
    out->height = info.extent.y;
    out->playCount = info.playCount;
    out->alpha = info.alpha;
    const uint32_t pixelSize = info.alpha ? 4 : 3;
    const size_t pixels = size_t(info.extent.x) * info.extent.y;
    Array<uint8_t> native(pixels * pixelSize);
    for (uint32_t frame = 0; frame < info.frameCount && status == PrStatus_Ok; ++frame) {
        status = prWebpDecoderRead(decoder, frame, PrPixels{native.data(), info.extent.x * pixelSize, info.extent});
        if (status != PrStatus_Ok)
            break;
        out->durations.push(prWebpDecoderFrameDuration(decoder, frame));
        const uint32_t at = out->rgba.count();
        out->rgba.resize(at + uint32_t(pixels * 4));
        for (size_t i = 0; i < pixels; ++i) {
            uint8_t* to = out->rgba.data() + at + i * 4;
            prMemCopy(to, native.data() + i * pixelSize, 3);
            to[3] = pixelSize == 4 ? native[uint32_t(i * 4 + 3)] : 0xFF;
        }
    }
    prWebpDecoderDestroy(decoder);
    return status;
}

PrStatus praxDecode(PrByteView file, Array<uint8_t>* scratch) {
    PrWebpDecoder* decoder = nullptr;
    PrStatus status = prWebpDecoderCreate(file, nullptr, file.size, &decoder);
    if (status == PrStatus_Ok) {
        const PrWebpInfo& info = *prWebpDecoderInfo(decoder);
        const uint32_t pixelSize = info.alpha ? 4 : 3;
        const uint32_t size = info.extent.x * info.extent.y * pixelSize;
        if (scratch->count() < size)
            scratch->resize(size);
        for (uint32_t frame = 0; frame < info.frameCount && status == PrStatus_Ok; ++frame)
            status =
                prWebpDecoderRead(decoder, frame, PrPixels{scratch->data(), info.extent.x * pixelSize, info.extent});
    }
    prWebpDecoderDestroy(decoder);
    return status;
}

bool referenceDecode(PrByteView file, Array<uint8_t>* scratch) {
    WebPBitstreamFeatures features = {};
    if (WebPGetFeatures(file.data, file.size, &features) != VP8_STATUS_OK)
        return false;
    if (!features.has_animation) {
        const uint32_t size = uint32_t(features.width * features.height * 4);
        if (scratch->count() < size)
            scratch->resize(size);
        return WebPDecodeRGBAInto(file.data, file.size, scratch->data(), size, features.width * 4) != nullptr;
    }
    Frames frames;
    return referenceFrames(file, &frames);
}

PrStatus praxPlanes(PrByteView file, Planes* out) {
    *out = Planes{};
    PrWebpDecoder* decoder = nullptr;
    PrStatus status = prWebpDecoderCreate(file, nullptr, file.size, &decoder);
    const PrUVec2 extent = status == PrStatus_Ok ? prWebpDecoderInfo(decoder)->extent : PrUVec2{};
    prWebpDecoderDestroy(decoder);
    if (status != PrStatus_Ok)
        return status;
    const PrUVec2 chroma = {(extent.x + 1) / 2, (extent.y + 1) / 2};
    out->width = extent.x;
    out->height = extent.y;
    out->y.resize(extent.x * extent.y);
    out->u.resize(chroma.x * chroma.y);
    out->v.resize(chroma.x * chroma.y);
    const PrWebpPlanes planes = {
        .luma = {out->y.data(), extent.x, extent},
        .blueDifference = {out->u.data(), chroma.x, chroma},
        .redDifference = {out->v.data(), chroma.x, chroma},
    };
    return prWebpDecodePlanes(file, nullptr, &planes);
}

Array<uint8_t> encode(const uint8_t* rgba, uint32_t width, uint32_t height, const Options& options) {
    Array<uint8_t> out;
    WebPConfig config = {};
    configure(options, &config);
    WebPPicture picture = {};
    if (!importPicture(rgba, width, height, options.lossless, &picture))
        return out;
    WebPMemoryWriter writer = {};
    WebPMemoryWriterInit(&writer);
    picture.writer = WebPMemoryWrite;
    picture.custom_ptr = &writer;
    if (WebPEncode(&config, &picture))
        out.append(writer.mem, uint32_t(writer.size));
    WebPMemoryWriterClear(&writer);
    WebPPictureFree(&picture);
    return out;
}

Array<uint8_t> encodeAnimation(const uint8_t* rgba, const uint32_t* durations, uint32_t count, uint32_t width,
        uint32_t height, const Options& options, const AnimationOptions& animation) {
    Array<uint8_t> out;
    WebPAnimEncoderOptions encoderOptions = {};
    if (!WebPAnimEncoderOptionsInit(&encoderOptions))
        return out;
    if (animation.kmin != 0 || animation.kmax != 0) {
        encoderOptions.kmin = animation.kmin;
        encoderOptions.kmax = animation.kmax;
    }
    encoderOptions.allow_mixed = animation.allowMixed ? 1 : 0;
    encoderOptions.minimize_size = animation.minimizeSize ? 1 : 0;
    encoderOptions.anim_params.loop_count = animation.loopCount;
    WebPAnimEncoder* encoder = WebPAnimEncoderNew(int(width), int(height), &encoderOptions);
    if (encoder == nullptr)
        return out;
    WebPConfig config = {};
    configure(options, &config);
    bool ok = true;
    int timestamp = 0;
    for (uint32_t frame = 0; frame < count && ok; ++frame) {
        WebPPicture picture = {};
        ok = importPicture(rgba + size_t(frame) * width * height * 4, width, height, options.lossless, &picture);
        ok = ok && WebPAnimEncoderAdd(encoder, &picture, timestamp, &config);
        WebPPictureFree(&picture);
        timestamp += int(durations[frame]);
    }
    WebPData data = {};
    WebPDataInit(&data);
    if (ok && WebPAnimEncoderAdd(encoder, nullptr, timestamp, nullptr) && WebPAnimEncoderAssemble(encoder, &data))
        out.append(data.bytes, uint32_t(data.size));
    WebPDataClear(&data);
    WebPAnimEncoderDelete(encoder);
    return out;
}

Array<uint8_t> withChunk(PrByteView file, const char* fourCc, PrByteView payload) {
    Array<uint8_t> out;
    const WebPData data = {file.data, file.size};
    WebPMux* mux = WebPMuxCreate(&data, 1);
    if (mux == nullptr)
        return out;
    const WebPData chunk = {payload.data, payload.size};
    WebPData assembled = {};
    WebPDataInit(&assembled);
    if (WebPMuxSetChunk(mux, fourCc, &chunk, 1) == WEBP_MUX_OK && WebPMuxAssemble(mux, &assembled) == WEBP_MUX_OK)
        out.append(assembled.bytes, uint32_t(assembled.size));
    WebPDataClear(&assembled);
    WebPMuxDelete(mux);
    return out;
}

} // namespace webp
