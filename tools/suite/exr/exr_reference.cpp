#include "tools/suite/exr/exr_reference.h"

#include "pr/exr.h"
#include "pr/mem.h"

#include "OpenEXRCore/openexr.h"
#include "tools/harness/harness_codecs.h"

using namespace harness;
using namespace pr;

namespace exr {

namespace {

// The bytes of a file in memory, which OpenEXR reads through its callbacks.
struct Memory {
    PrByteView file;
};

int64_t readMemory(exr_const_context_t, void* userdata, void* buffer, uint64_t size, uint64_t offset,
        exr_stream_error_func_ptr_t) {
    const Memory* memory = static_cast<const Memory*>(userdata);
    if (offset >= memory->file.size)
        return 0;
    uint64_t available = memory->file.size - offset;
    uint64_t read = size < available ? size : available;
    prMemCopy(buffer, memory->file.data + offset, size_t(read));
    return int64_t(read);
}

int64_t sizeMemory(exr_const_context_t, void* userdata) {
    return int64_t(static_cast<const Memory*>(userdata)->file.size);
}

void quiet(exr_const_context_t, exr_result_t, const char*) {}

// Opens the file with OpenEXR. Returns null if it refuses the header.
exr_context_t openReference(Memory* memory) {
    exr_context_initializer_t init = EXR_DEFAULT_CONTEXT_INITIALIZER;
    init.user_data = memory;
    init.read_fn = readMemory;
    init.size_fn = sizeMemory;
    init.error_handler_fn = quiet;
    init.flags = EXR_CONTEXT_FLAG_SILENT_HEADER_PARSE;
    exr_context_t context = nullptr;
    if (exr_start_read(&context, "memory", &init) != EXR_ERR_SUCCESS) {
        exr_finish(&context);
        return nullptr;
    }
    return context;
}

// Returns the number of lines from `y` through `y + height - 1` that a channel of `sampling` has samples on.
uint32_t sampledLines(int64_t y, int64_t height, int64_t sampling) {
    if (sampling == 1 || height <= 0)
        return uint32_t(height > 0 ? height : 0);
    int64_t last = y + height - 1;
    int64_t high = last >= 0 ? last / sampling : -((-last + sampling - 1) / sampling);
    int64_t low = y >= 0 ? (y + sampling - 1) / sampling : -((-y) / sampling);
    return high >= low ? uint32_t(high - low + 1) : 0;
}

// The levels of a tiled part as lib/exr numbers them, by their indices across and down.
struct LevelIndex {
    int32_t x;
    int32_t y;
};

Array<LevelIndex> levelsOf(exr_context_t context, int part, bool tiled) {
    Array<LevelIndex> levels;
    if (!tiled) {
        levels.push(LevelIndex{0, 0});
        return levels;
    }
    int32_t across = 0;
    int32_t down = 0;
    uint32_t tileWidth = 0;
    uint32_t tileHeight = 0;
    exr_tile_level_mode_t mode = EXR_TILE_ONE_LEVEL;
    exr_tile_round_mode_t round = EXR_TILE_ROUND_DOWN;
    if (exr_get_tile_levels(context, part, &across, &down) != EXR_ERR_SUCCESS ||
            exr_get_tile_descriptor(context, part, &tileWidth, &tileHeight, &mode, &round) != EXR_ERR_SUCCESS)
        return levels;
    for (int32_t y = 0; y < down; ++y) {
        for (int32_t x = 0; x < across; ++x) {
            if (mode != EXR_TILE_RIPMAP_LEVELS && x != y)
                continue;
            levels.push(LevelIndex{x, y});
        }
    }
    return levels;
}

// Decodes one chunk into the level's planes. A plane's samples of the chunk start at its row `row`, and each line of
// the level is `stride` samples apart. Returns false if OpenEXR refuses the chunk.
bool decodeChunk(exr_context_t context, int part, const exr_chunk_info_t& chunk, exr_decode_pipeline_t* decoder,
        bool* initialized, Level* level, int32_t x, int32_t y) {
    if (!*initialized) {
        if (exr_decoding_initialize(context, part, &chunk, decoder) != EXR_ERR_SUCCESS)
            return false;
        *initialized = true;
    } else if (exr_decoding_update(context, part, &chunk, decoder) != EXR_ERR_SUCCESS) {
        return false;
    }
    for (int c = 0; c < decoder->channel_count; ++c) {
        exr_coding_channel_info_t& channel = decoder->channels[c];
        Plane& plane = level->planes[uint32_t(c)];
        if (channel.height == 0 || channel.width == 0) {
            channel.decode_to_ptr = nullptr;
            continue;
        }
        uint32_t row = sampledLines(level->y, int64_t(y) - level->y, plane.ySampling);
        uint32_t column = uint32_t((int64_t(x) - level->x) / plane.xSampling);
        channel.decode_to_ptr = plane.samples.data() + (size_t(row) * plane.width + column) * plane.sampleBytes;
        channel.user_pixel_stride = int32_t(plane.sampleBytes);
        channel.user_line_stride = int32_t(plane.width * plane.sampleBytes);
        channel.user_bytes_per_element = int16_t(plane.sampleBytes);
        channel.user_data_type = channel.data_type;
    }
    if (exr_decoding_choose_default_routines(context, part, decoder) != EXR_ERR_SUCCESS)
        return false;
    return exr_decoding_run(context, part, decoder) == EXR_ERR_SUCCESS;
}

// Decodes a part of the open file into `out`. Returns false if OpenEXR refuses any of its chunks.
bool decodePart(exr_context_t context, int part, Part* out) {
    exr_storage_t storage = EXR_STORAGE_SCANLINE;
    exr_attr_box2i_t data{};
    exr_attr_box2i_t display{};
    exr_compression_t compression = EXR_COMPRESSION_NONE;
    const exr_attr_chlist_t* channels = nullptr;
    if (exr_get_storage(context, part, &storage) != EXR_ERR_SUCCESS ||
            exr_get_data_window(context, part, &data) != EXR_ERR_SUCCESS ||
            exr_get_display_window(context, part, &display) != EXR_ERR_SUCCESS ||
            exr_get_compression(context, part, &compression) != EXR_ERR_SUCCESS ||
            exr_get_channels(context, part, &channels) != EXR_ERR_SUCCESS)
        return false;
    out->displayX = display.min.x;
    out->displayY = display.min.y;
    out->displayWidth = uint32_t(int64_t(display.max.x) - display.min.x + 1);
    out->displayHeight = uint32_t(int64_t(display.max.y) - display.min.y + 1);
    out->compression = uint32_t(compression);
    if (storage != EXR_STORAGE_SCANLINE && storage != EXR_STORAGE_TILED)
        return false;
    bool tiled = storage == EXR_STORAGE_TILED;
    for (const LevelIndex& index : levelsOf(context, part, tiled)) {
        Level level;
        level.x = data.min.x;
        level.y = data.min.y;
        int32_t width = int32_t(int64_t(data.max.x) - data.min.x + 1);
        int32_t height = int32_t(int64_t(data.max.y) - data.min.y + 1);
        if (tiled && exr_get_level_sizes(context, part, index.x, index.y, &width, &height) != EXR_ERR_SUCCESS)
            return false;
        level.width = uint32_t(width);
        level.height = uint32_t(height);
        for (int c = 0; c < channels->num_channels; ++c) {
            const exr_attr_chlist_entry_t& entry = channels->entries[c];
            Plane plane;
            plane.name = PrStringView(entry.name.str, size_t(entry.name.length));
            plane.pixel = uint32_t(entry.pixel_type);
            plane.linear = entry.p_linear != 0;
            plane.xSampling = uint32_t(entry.x_sampling);
            plane.ySampling = uint32_t(entry.y_sampling);
            plane.sampleBytes = entry.pixel_type == EXR_PIXEL_HALF ? 2 : 4;
            plane.width = level.width / plane.xSampling;
            plane.height = sampledLines(level.y, level.height, plane.ySampling);
            plane.samples.resize(plane.width * plane.height * plane.sampleBytes);
            level.planes.push(static_cast<Plane&&>(plane));
        }
        exr_decode_pipeline_t decoder = EXR_DECODE_PIPELINE_INITIALIZER;
        bool initialized = false;
        bool ok = true;
        if (!tiled) {
            int32_t lines = 0;
            ok = exr_get_scanlines_per_chunk(context, part, &lines) == EXR_ERR_SUCCESS && lines > 0;
            for (int64_t y = data.min.y; ok && y <= data.max.y; y += lines) {
                exr_chunk_info_t chunk{};
                ok = exr_read_scanline_chunk_info(context, part, int(y), &chunk) == EXR_ERR_SUCCESS &&
                    decodeChunk(context, part, chunk, &decoder, &initialized, &level, data.min.x, int32_t(y));
            }
        } else {
            int32_t tileWidth = 0;
            int32_t tileHeight = 0;
            ok = exr_get_tile_sizes(context, part, index.x, index.y, &tileWidth, &tileHeight) == EXR_ERR_SUCCESS &&
                tileWidth > 0 && tileHeight > 0;
            for (int32_t ty = 0; ok && int64_t(ty) * tileHeight < height; ++ty) {
                for (int32_t tx = 0; ok && int64_t(tx) * tileWidth < width; ++tx) {
                    exr_chunk_info_t chunk{};
                    ok = exr_read_tile_chunk_info(context, part, tx, ty, index.x, index.y, &chunk) == EXR_ERR_SUCCESS &&
                        decodeChunk(context, part, chunk, &decoder, &initialized, &level,
                            int32_t(int64_t(data.min.x) + int64_t(tx) * tileWidth),
                            int32_t(int64_t(data.min.y) + int64_t(ty) * tileHeight));
                }
            }
        }
        exr_decoding_destroy(context, &decoder);
        if (!ok)
            return false;
        out->levels.push(static_cast<Level&&>(level));
    }
    return true;
}

// Decodes the selected part of the open image into `out`.
PrStatus praxPart(PrExrImage* image, Part* out) {
    const PrExrPartInfo& info = *prExrImagePartInfo(image);
    PrExrHeader header{};
    PrStatus status = prExrImageHeaderRead(image, &header);
    if (status != PrStatus_Ok)
        return status;
    out->displayX = header.displayWindow.xMin;
    out->displayY = header.displayWindow.yMin;
    out->displayWidth = info.extent.x;
    out->displayHeight = info.extent.y;
    out->compression = header.compression;
    Array<uint8_t> samples;
    for (uint32_t index = 0; index < info.levelCount; ++index) {
        PrUVec2 extent = prExrImageLevelExtent(image, index);
        Level level;
        level.x = header.dataWindow.xMin;
        level.y = header.dataWindow.yMin;
        if (index == 0) {
            level.width = uint32_t(int64_t(header.dataWindow.xMax) - header.dataWindow.xMin + 1);
            level.height = uint32_t(int64_t(header.dataWindow.yMax) - header.dataWindow.yMin + 1);
        } else {
            level.width = extent.x;
            level.height = extent.y;
        }
        // The output's first pixel: the display window's at the first level, and the data window's at the rest.
        int64_t originX = index == 0 ? header.displayWindow.xMin : header.dataWindow.xMin;
        int64_t originY = index == 0 ? header.displayWindow.yMin : header.dataWindow.yMin;
        for (uint32_t c = 0; c < info.channelCount; ++c) {
            PrExrChannel channel = prExrImageChannel(image, c);
            Plane plane;
            plane.name = channel.name;
            plane.pixel = channel.sample;
            plane.linear = channel.perceptual;
            plane.xSampling = channel.sampling.x;
            plane.ySampling = channel.sampling.y;
            plane.sampleBytes = channel.sample == PrExrSample_Half ? 2 : 4;
            plane.width = level.width / plane.xSampling;
            plane.height = sampledLines(level.y, level.height, plane.ySampling);
            plane.samples.resize(plane.width * plane.height * plane.sampleBytes);
            prMemZero(plane.samples.data(), plane.samples.count());
            samples.resize(uint32_t(size_t(extent.x) * extent.y * plane.sampleBytes));
            PrPixels pixels{samples.data(), size_t(extent.x) * plane.sampleBytes, extent};
            PrStatus read = prExrImageReadChannels(image, index, &c, 1, channel.sample, pixels);
            if (read != PrStatus_Ok && read != PrStatus_Truncated)
                return read;
            status = read == PrStatus_Truncated ? read : status;
            // The first line with samples, and then each sampled line.
            int64_t firstLine = level.y;
            while (plane.ySampling != 1 && firstLine % int64_t(plane.ySampling) != 0)
                ++firstLine;
            for (uint32_t row = 0; row < plane.height; ++row) {
                int64_t py = firstLine + int64_t(row) * plane.ySampling - originY;
                if (py < 0 || py >= extent.y)
                    continue;
                for (uint32_t column = 0; column < plane.width; ++column) {
                    int64_t px = level.x + int64_t(column) * plane.xSampling - originX;
                    if (px < 0 || px >= extent.x)
                        continue;
                    prMemCopy(plane.samples.data() + (size_t(row) * plane.width + column) * plane.sampleBytes,
                        samples.data() + (size_t(py) * extent.x + size_t(px)) * plane.sampleBytes, plane.sampleBytes);
                }
            }
            level.planes.push(static_cast<Plane&&>(plane));
        }
        out->levels.push(static_cast<Level&&>(level));
    }
    return status;
}

} // namespace

bool referenceImage(PrByteView file, Image* out) {
    *out = Image{};
    Memory memory{file};
    exr_context_t context = openReference(&memory);
    if (context == nullptr)
        return false;
    int count = 0;
    bool ok = exr_get_count(context, &count) == EXR_ERR_SUCCESS;
    for (int part = 0; ok && part < count; ++part) {
        Part decoded;
        decoded.decoded = decodePart(context, part, &decoded);
        exr_storage_t storage = EXR_STORAGE_SCANLINE;
        exr_get_storage(context, part, &storage);
        bool deep = storage == EXR_STORAGE_DEEP_SCANLINE || storage == EXR_STORAGE_DEEP_TILED;
        decoded.deep = deep;
        // A part that is not deep data and does not decode makes the file one that OpenEXR refuses.
        ok = decoded.decoded || deep;
        if (!decoded.decoded)
            decoded.levels.clear();
        out->parts.push(static_cast<Part&&>(decoded));
    }
    exr_finish(&context);
    return ok;
}

PrStatus praxImage(PrByteView file, Image* out) {
    *out = Image{};
    PrExrImage* image = nullptr;
    PrStatus status = prExrImageCreate(file, harnessCompress(), nullptr, file.size, &image);
    uint32_t parts = image != nullptr ? prExrImagePartCount(image) : 0;
    bool truncated = status == PrStatus_Truncated;
    for (uint32_t part = 0; part < parts; ++part) {
        Part decoded;
        PrStatus selected = prExrImageSelectPart(image, part);
        // A part that lib/exr does not read, such as deep data, is not decoded, and the part before stays selected.
        if (selected == PrStatus_NotSupported) {
            decoded.decoded = false;
            out->parts.push(static_cast<Part&&>(decoded));
            continue;
        }
        status = selected == PrStatus_Ok || selected == PrStatus_Truncated ? praxPart(image, &decoded) : selected;
        truncated = truncated || selected == PrStatus_Truncated;
        if (status != PrStatus_Ok && status != PrStatus_Truncated)
            break;
        out->parts.push(static_cast<Part&&>(decoded));
    }
    prExrImageDestroy(image);
    return status == PrStatus_Ok && truncated ? PrStatus_Truncated : status;
}

Comparison compare(const Image& prax, const Image& reference) {
    Comparison result;
    if (prax.parts.count() != reference.parts.count()) {
        result.shapes = false;
        return result;
    }
    for (uint32_t p = 0; p < prax.parts.count(); ++p) {
        const Part& a = prax.parts[p];
        const Part& b = reference.parts[p];
        if (a.decoded != b.decoded || a.levels.count() != b.levels.count()) {
            result.shapes = false;
            continue;
        }
        for (uint32_t l = 0; l < a.levels.count(); ++l) {
            const Level& la = a.levels[l];
            const Level& lb = b.levels[l];
            if (la.width != lb.width || la.height != lb.height || la.planes.count() != lb.planes.count()) {
                result.shapes = false;
                continue;
            }
            for (uint32_t c = 0; c < la.planes.count(); ++c) {
                const Plane& pa = la.planes[c];
                const Plane& pb = lb.planes[c];
                if (pa.width != pb.width || pa.height != pb.height || pa.sampleBytes != pb.sampleBytes) {
                    result.shapes = false;
                    continue;
                }
                int64_t firstLine = la.y;
                while (pa.ySampling != 1 && firstLine % int64_t(pa.ySampling) != 0)
                    ++firstLine;
                for (uint32_t row = 0; row < pa.height; ++row) {
                    int64_t y = firstLine + int64_t(row) * pa.ySampling;
                    for (uint32_t column = 0; column < pa.width; ++column) {
                        int64_t x = la.x + int64_t(column) * pa.xSampling;
                        if (l == 0 && (x < a.displayX || x >= int64_t(a.displayX) + a.displayWidth ||
                                y < a.displayY || y >= int64_t(a.displayY) + a.displayHeight)) {
                            result.outside += 1;
                            continue;
                        }
                        size_t at = (size_t(row) * pa.width + column) * pa.sampleBytes;
                        result.compared += 1;
                        if (!prMemEquals(pa.samples.data() + at, pb.samples.data() + at, pa.sampleBytes))
                            result.differ += 1;
                    }
                }
            }
        }
    }
    return result;
}

PrStatus praxDecode(PrByteView file, Array<uint8_t>* scratch) {
    PrExrImage* image = nullptr;
    PrStatus status = prExrImageCreate(file, harnessCompress(), nullptr, file.size, &image);
    uint32_t parts = status == PrStatus_Ok ? prExrImagePartCount(image) : 0;
    for (uint32_t part = 0; part < parts && status == PrStatus_Ok; ++part) {
        if (part != 0)
            status = prExrImageSelectPart(image, part);
        if (status == PrStatus_NotSupported) {
            status = PrStatus_Ok;
            continue;
        }
        const PrExrPartInfo& info = *prExrImagePartInfo(image);
        uint32_t pixelSize = (info.colors + (info.alpha ? 1 : 0)) * (info.sample == PrExrSample_Half ? 2 : 4);
        for (uint32_t level = 0; level < info.levelCount && status == PrStatus_Ok; ++level) {
            PrUVec2 extent = prExrImageLevelExtent(image, level);
            scratch->resize(uint32_t(size_t(extent.x) * extent.y * pixelSize));
            status = prExrImageRead(image, level, PrPixels{scratch->data(), size_t(extent.x) * pixelSize, extent});
        }
    }
    prExrImageDestroy(image);
    return status;
}

bool referenceDecode(PrByteView file, Array<uint8_t>* scratch) {
    Image image;
    bool ok = referenceImage(file, &image);
    scratch->resize(1);
    return ok;
}

Array<uint8_t> encode(const Image& image, uint32_t compression, Layout layout) {
    Array<uint8_t> written;
    if (image.parts.count() == 0 || image.parts[0].levels.count() == 0)
        return written;
    const Level& first = image.parts[0].levels[0];
    bool tiled = layout != Layout_ScanLines;

    struct Sink {
        Array<uint8_t>* bytes;
    } sink{&written};
    exr_context_initializer_t init = EXR_DEFAULT_CONTEXT_INITIALIZER;
    init.user_data = &sink;
    init.error_handler_fn = quiet;
    init.write_fn = [](exr_const_context_t, void* userdata, const void* buffer, uint64_t size, uint64_t offset,
            exr_stream_error_func_ptr_t) -> int64_t {
        Array<uint8_t>* bytes = static_cast<Sink*>(userdata)->bytes;
        if (offset + size > bytes->count())
            bytes->resize(uint32_t(offset + size));
        prMemCopy(bytes->data() + offset, buffer, size_t(size));
        return int64_t(size);
    };
    exr_context_t context = nullptr;
    bool ok = exr_start_write(&context, "memory", EXR_WRITE_FILE_DIRECTLY, &init) == EXR_ERR_SUCCESS;
    int part = 0;
    ok = ok && exr_add_part(context, "image", tiled ? EXR_STORAGE_TILED : EXR_STORAGE_SCANLINE, &part) ==
        EXR_ERR_SUCCESS;
    ok = ok && exr_initialize_required_attr_simple(context, part, int32_t(first.width), int32_t(first.height),
        exr_compression_t(compression)) == EXR_ERR_SUCCESS;
    if (tiled) {
        exr_tile_level_mode_t mode = EXR_TILE_ONE_LEVEL;
        if (layout == Layout_MipTiles)
            mode = EXR_TILE_MIPMAP_LEVELS;
        else if (layout == Layout_RipTiles)
            mode = EXR_TILE_RIPMAP_LEVELS;
        ok = ok && exr_set_tile_descriptor(context, part, 64, 64, mode, EXR_TILE_ROUND_DOWN) == EXR_ERR_SUCCESS;
    }
    for (uint32_t c = 0; ok && c < first.planes.count(); ++c) {
        const Plane& plane = first.planes[c];
        if (tiled && (plane.xSampling != 1 || plane.ySampling != 1))
            ok = false;
        char name[256] = {};
        uint32_t length = plane.name.count() < 255 ? plane.name.count() : 255;
        prMemCopy(name, plane.name.data(), length);
        ok = ok && exr_add_channel(context, part, name, exr_pixel_type_t(plane.pixel),
            plane.linear ? EXR_PERCEPTUALLY_LINEAR : EXR_PERCEPTUALLY_LOGARITHMIC, int32_t(plane.xSampling),
            int32_t(plane.ySampling)) == EXR_ERR_SUCCESS;
    }
    ok = ok && exr_write_header(context) == EXR_ERR_SUCCESS;

    // The planes of each level: the first level's samples at every 2^level pixel across and down.
    Array<LevelIndex> levels = ok ? levelsOf(context, part, tiled) : Array<LevelIndex>();
    for (const LevelIndex& index : levels) {
        if (!ok)
            break;
        int32_t width = int32_t(first.width);
        int32_t height = int32_t(first.height);
        if (tiled)
            ok = exr_get_level_sizes(context, part, index.x, index.y, &width, &height) == EXR_ERR_SUCCESS;
        Array<Array<uint8_t>> planes;
        for (uint32_t c = 0; ok && c < first.planes.count(); ++c) {
            const Plane& plane = first.planes[c];
            Array<uint8_t> samples;
            uint32_t planeWidth = uint32_t(width) / plane.xSampling;
            uint32_t planeHeight = sampledLines(0, height, plane.ySampling);
            samples.resize(planeWidth * planeHeight * plane.sampleBytes);
            for (uint32_t y = 0; y < planeHeight; ++y) {
                for (uint32_t x = 0; x < planeWidth; ++x) {
                    uint32_t sx = x << index.x;
                    uint32_t sy = y << index.y;
                    sx = sx < plane.width ? sx : plane.width - 1;
                    sy = sy < plane.height ? sy : plane.height - 1;
                    prMemCopy(samples.data() + (size_t(y) * planeWidth + x) * plane.sampleBytes,
                        plane.samples.data() + (size_t(sy) * plane.width + sx) * plane.sampleBytes, plane.sampleBytes);
                }
            }
            planes.push(static_cast<Array<uint8_t>&&>(samples));
        }
        exr_encode_pipeline_t encoder{};
        bool initialized = false;
        auto chunk = [&](const exr_chunk_info_t& info, int32_t x, int32_t y) {
            if (!initialized) {
                ok = exr_encoding_initialize(context, part, &info, &encoder) == EXR_ERR_SUCCESS;
            } else {
                ok = exr_encoding_update(context, part, &info, &encoder) == EXR_ERR_SUCCESS;
            }
            for (int c = 0; ok && c < encoder.channel_count; ++c) {
                exr_coding_channel_info_t& channel = encoder.channels[c];
                const Plane& plane = first.planes[uint32_t(c)];
                uint32_t planeWidth = uint32_t(width) / plane.xSampling;
                if (channel.height == 0) {
                    channel.encode_from_ptr = nullptr;
                    channel.user_pixel_stride = 0;
                    channel.user_line_stride = 0;
                    continue;
                }
                uint32_t row = sampledLines(0, y, plane.ySampling);
                channel.encode_from_ptr = planes[uint32_t(c)].data() +
                    (size_t(row) * planeWidth + uint32_t(x) / plane.xSampling) * plane.sampleBytes;
                channel.user_pixel_stride = int32_t(plane.sampleBytes);
                channel.user_line_stride = int32_t(planeWidth * plane.sampleBytes);
            }
            if (ok && !initialized)
                ok = exr_encoding_choose_default_routines(context, part, &encoder) == EXR_ERR_SUCCESS;
            initialized = true;
            ok = ok && exr_encoding_run(context, part, &encoder) == EXR_ERR_SUCCESS;
        };
        if (!tiled) {
            int32_t lines = 0;
            ok = ok && exr_get_scanlines_per_chunk(context, part, &lines) == EXR_ERR_SUCCESS;
            for (int32_t y = 0; ok && y < height; y += lines) {
                exr_chunk_info_t info{};
                ok = exr_write_scanline_chunk_info(context, part, y, &info) == EXR_ERR_SUCCESS;
                if (ok)
                    chunk(info, 0, y);
            }
        } else {
            for (int32_t ty = 0; ok && ty * 64 < height; ++ty) {
                for (int32_t tx = 0; ok && tx * 64 < width; ++tx) {
                    exr_chunk_info_t info{};
                    ok = exr_write_tile_chunk_info(context, part, tx, ty, index.x, index.y, &info) == EXR_ERR_SUCCESS;
                    if (ok)
                        chunk(info, tx * 64, ty * 64);
                }
            }
        }
        if (initialized)
            exr_encoding_destroy(context, &encoder);
    }
    if (exr_finish(&context) != EXR_ERR_SUCCESS)
        ok = false;
    if (!ok)
        written.clear();
    return written;
}

} // namespace exr
