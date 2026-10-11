#include "format.h"

namespace webdx {

FormatLayout LayoutOf(D3DFORMAT format)
{
    switch ((DWORD)format) {
    case D3DFMT_DXT1: return {4, 8};
    case D3DFMT_DXT3: case D3DFMT_DXT5: case MAKEFOURCC('A', 'T', 'I', '2'): return {4, 16};
    case D3DFMT_A8: case D3DFMT_L8: return {1, 1};
    case D3DFMT_R5G6B5: case D3DFMT_X1R5G5B5: case D3DFMT_A1R5G5B5: case D3DFMT_A4R4G4B4:
    case D3DFMT_A8L8: case D3DFMT_D16: case D3DFMT_D16_LOCKABLE: case D3DFMT_D15S1:
        return {1, 2};
    case D3DFMT_R8G8B8: return {1, 3};
    case D3DFMT_A16B16G16R16: case D3DFMT_A16B16G16R16F: return {1, 8};
    default: return {1, 4};
    }
}

} // namespace webdx
