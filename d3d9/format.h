// format.h — memory layout of Direct3D 9 surface formats and their GPU counterparts.
#pragma once

#include "executor.h"
#include "stream.h"

#include <d3d9.h>
#include <vector>

namespace webdx {

struct FormatLayout {
    UINT block;          // 1 for plain formats, 4 for DXT
    UINT bytesPerBlock;
};

FormatLayout LayoutOf(D3DFORMAT format);

inline UINT RowPitch(D3DFORMAT format, UINT width)
{
    FormatLayout l = LayoutOf(format);
    return (width + l.block - 1) / l.block * l.bytesPerBlock;
}

inline UINT RowCount(D3DFORMAT format, UINT height)
{
    FormatLayout l = LayoutOf(format);
    return (height + l.block - 1) / l.block;
}

inline UINT MipSize(UINT size, UINT level) { return size >> level ? size >> level : 1; }

// TexFormat::None for surfaces without data of their own (D3DFMT_UNKNOWN, the 'NULL' render target).
TexFormat GpuFormatOf(D3DFORMAT format, const Features &features);

// Appends `rows` rows of `width` texels (block rows and block-aligned texels for DXT) converted to
// GpuFormatOf(format) on a GPU with every feature; returns the converted row size, 0 when the format
// has no conversion.
UINT ConvertRows(D3DFORMAT format, const uint8_t *src, UINT srcPitch, UINT width, UINT rows,
                 std::vector<uint8_t> &out);

// Decodes the DXT blocks covering `width` x `height` texels (multiples of 4) to RGBA8 and appends the
// top-left `outWidth` x `outHeight` of them; returns the appended row size.
UINT DecodeDxt(D3DFORMAT format, const uint8_t *src, UINT srcPitch, UINT width, UINT height, UINT outWidth,
               UINT outHeight, std::vector<uint8_t> &out);

} // namespace webdx
