#include "format.h"

#include <cstring>

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

TexFormat GpuFormatOf(D3DFORMAT format)
{
    switch ((DWORD)format) {
    case D3DFMT_A8R8G8B8: case D3DFMT_X8R8G8B8: return TexFormat::BGRA8;
    case D3DFMT_R5G6B5: case D3DFMT_L8: case D3DFMT_A8L8: return TexFormat::RGBA8;
    case D3DFMT_G16R16: return TexFormat::RG32F;
    case D3DFMT_A16B16G16R16: case D3DFMT_A16B16G16R16F: return TexFormat::RGBA16F;
    case D3DFMT_R32F: return TexFormat::R32F;
    case D3DFMT_DXT1: return TexFormat::BC1;
    case D3DFMT_DXT3: return TexFormat::BC2;
    case D3DFMT_DXT5: return TexFormat::BC3;
    case D3DFMT_D24S8: case D3DFMT_D24X8: case D3DFMT_D16: return TexFormat::Depth24S8;
    case D3DFMT_D24FS8: return TexFormat::Depth32FS8;
    case D3DFMT_UNKNOWN: case MAKEFOURCC('N', 'U', 'L', 'L'): return TexFormat::None;
    default: return TexFormat::BGRA8;
    }
}

static uint8_t Expand(uint32_t v, int bits) { return (uint8_t)(v * 255 / ((1u << bits) - 1)); }

UINT ConvertRows(D3DFORMAT format, const uint8_t *src, UINT srcPitch, UINT width, UINT rows,
                 std::vector<uint8_t> &out)
{
    UINT rowSize;
    switch ((DWORD)format) {
    case D3DFMT_A8R8G8B8: case D3DFMT_X8R8G8B8: case D3DFMT_R5G6B5: case D3DFMT_L8: case D3DFMT_A8L8:
    case D3DFMT_R32F:
        rowSize = width * 4;
        break;
    case D3DFMT_G16R16: case D3DFMT_A16B16G16R16F: rowSize = width * 8; break;
    case D3DFMT_DXT1: case D3DFMT_DXT3: case D3DFMT_DXT5:
        rowSize = RowPitch(format, width);
        break;
    default: return 0;
    }
    size_t at = out.size();
    out.resize(at + (size_t)rowSize * rows);
    for (UINT y = 0; y < rows; ++y, src += srcPitch) {
        uint8_t *d = out.data() + at + (size_t)y * rowSize;
        switch ((DWORD)format) {
        case D3DFMT_X8R8G8B8:
            std::memcpy(d, src, rowSize);
            for (UINT x = 0; x < width; ++x) d[4 * x + 3] = 255;
            break;
        case D3DFMT_R5G6B5:
            for (UINT x = 0; x < width; ++x) {
                uint32_t v = src[2 * x] | src[2 * x + 1] << 8;
                d[4 * x] = Expand(v >> 11, 5);
                d[4 * x + 1] = Expand((v >> 5) & 63, 6);
                d[4 * x + 2] = Expand(v & 31, 5);
                d[4 * x + 3] = 255;
            }
            break;
        case D3DFMT_L8:
            for (UINT x = 0; x < width; ++x) {
                d[4 * x] = d[4 * x + 1] = d[4 * x + 2] = src[x];
                d[4 * x + 3] = 255;
            }
            break;
        case D3DFMT_A8L8:
            for (UINT x = 0; x < width; ++x) {
                d[4 * x] = d[4 * x + 1] = d[4 * x + 2] = src[2 * x];
                d[4 * x + 3] = src[2 * x + 1];
            }
            break;
        case D3DFMT_G16R16:
            for (UINT x = 0; x < 2 * width; ++x) {
                float f = (src[2 * x] | src[2 * x + 1] << 8) / 65535.0f;
                std::memcpy(d + 4 * x, &f, 4);
            }
            break;
        default:
            std::memcpy(d, src, rowSize);
            break;
        }
    }
    return rowSize;
}

} // namespace webdx
