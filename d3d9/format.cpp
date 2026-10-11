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

TexFormat GpuFormatOf(D3DFORMAT format, const Features &features)
{
    switch ((DWORD)format) {
    case D3DFMT_A8R8G8B8: case D3DFMT_X8R8G8B8: return TexFormat::BGRA8;
    case D3DFMT_R5G6B5: case D3DFMT_L8: case D3DFMT_A8L8: return TexFormat::RGBA8;
    case D3DFMT_G16R16: return TexFormat::RG32F;
    case D3DFMT_A16B16G16R16: case D3DFMT_A16B16G16R16F: return TexFormat::RGBA16F;
    case D3DFMT_R32F: return TexFormat::R32F;
    case D3DFMT_DXT1: return features.bc ? TexFormat::BC1 : TexFormat::RGBA8;
    case D3DFMT_DXT3: return features.bc ? TexFormat::BC2 : TexFormat::RGBA8;
    case D3DFMT_DXT5: return features.bc ? TexFormat::BC3 : TexFormat::RGBA8;
    case D3DFMT_D24S8: case D3DFMT_D24X8: case D3DFMT_D16: return TexFormat::Depth24S8;
    case D3DFMT_D24FS8: return features.depth32Stencil8 ? TexFormat::Depth32FS8 : TexFormat::Depth24S8;
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

static void Color565(uint32_t c, uint8_t *rgba)
{
    rgba[0] = Expand(c >> 11, 5);
    rgba[1] = Expand((c >> 5) & 63, 6);
    rgba[2] = Expand(c & 31, 5);
    rgba[3] = 255;
}

// One 4x4 color block into `pixels` (16 RGBA texels, row-major).
static void DecodeColorBlock(const uint8_t *b, bool dxt1, uint8_t *pixels)
{
    uint32_t c0 = b[0] | b[1] << 8, c1 = b[2] | b[3] << 8;
    uint8_t palette[4][4];
    Color565(c0, palette[0]);
    Color565(c1, palette[1]);
    for (int k = 0; k < 3; ++k) {
        if (!dxt1 || c0 > c1) {
            palette[2][k] = (uint8_t)((2 * palette[0][k] + palette[1][k]) / 3);
            palette[3][k] = (uint8_t)((palette[0][k] + 2 * palette[1][k]) / 3);
        } else {
            palette[2][k] = (uint8_t)((palette[0][k] + palette[1][k]) / 2);
            palette[3][k] = 0;
        }
    }
    palette[2][3] = 255;
    palette[3][3] = !dxt1 || c0 > c1 ? 255 : 0;
    uint32_t indices = b[4] | b[5] << 8 | b[6] << 16 | (uint32_t)b[7] << 24;
    for (int i = 0; i < 16; ++i) std::memcpy(pixels + 4 * i, palette[(indices >> (2 * i)) & 3], 4);
}

UINT DecodeDxt(D3DFORMAT format, const uint8_t *src, UINT srcPitch, UINT width, UINT height, UINT outWidth,
               UINT outHeight, std::vector<uint8_t> &out)
{
    UINT blockBytes = LayoutOf(format).bytesPerBlock;
    std::vector<uint8_t> decoded((size_t)width * height * 4);
    uint8_t pixels[64];
    for (UINT by = 0; by < height / 4; ++by) {
        for (UINT bx = 0; bx < width / 4; ++bx) {
            const uint8_t *b = src + (size_t)by * srcPitch + (size_t)bx * blockBytes;
            if ((DWORD)format == D3DFMT_DXT1) {
                DecodeColorBlock(b, true, pixels);
            } else {
                DecodeColorBlock(b + 8, false, pixels);
                if ((DWORD)format == D3DFMT_DXT3) {
                    for (int i = 0; i < 16; ++i) pixels[4 * i + 3] = (uint8_t)(((b[i / 2] >> (4 * (i & 1))) & 15) * 17);
                } else {
                    uint32_t a0 = b[0], a1 = b[1], alpha[8] = {a0, a1};
                    for (int k = 1; k < 7; ++k) alpha[k + 1] = a0 > a1 ? ((7 - k) * a0 + k * a1) / 7 : k < 5 ? ((5 - k) * a0 + k * a1) / 5 : k == 5 ? 0 : 255;
                    uint64_t indices = 0;
                    for (int i = 0; i < 6; ++i) indices |= (uint64_t)b[2 + i] << (8 * i);
                    for (int i = 0; i < 16; ++i) pixels[4 * i + 3] = (uint8_t)alpha[(indices >> (3 * i)) & 7];
                }
            }
            for (int row = 0; row < 4; ++row)
                std::memcpy(decoded.data() + ((size_t)(by * 4 + row) * width + bx * 4) * 4, pixels + 16 * row, 16);
        }
    }
    for (UINT y = 0; y < outHeight; ++y) {
        const uint8_t *row = decoded.data() + (size_t)y * width * 4;
        out.insert(out.end(), row, row + outWidth * 4);
    }
    return outWidth * 4;
}

} // namespace webdx
