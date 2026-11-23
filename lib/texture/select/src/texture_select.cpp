// The selector for the formats of lib/texture.

#include "pr/texture_select.h"

#include "pr/dds_texture_source.h"
#include "pr/image_texture_source.h"
#include "pr/ktx_texture_source.h"

PrTextureFormatSet* prTextureFormatSetCreateDefault(const PrCompressCodecSet* PR_NULLABLE compress,
        const PrImageCodecSet* PR_NULLABLE images, PrImageTextureColor color) {
    PrTextureFormat* formats[3];
    uint32_t count = 0;
    formats[count++] = prKtxTextureFormatCreate(compress);
    formats[count++] = prDdsTextureFormatCreate();
    if (images != nullptr)
        formats[count++] = prImageTextureFormatCreate(images, color);
    return prTextureFormatSetCreate(formats, count);
}
