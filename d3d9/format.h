// format.h — memory layout of Direct3D 9 surface formats.
#pragma once

#include <d3d9.h>

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

} // namespace webdx
