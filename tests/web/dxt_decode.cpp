// DecodeDxt against texels worked out by hand from the block formats. Runs under Node: node dxt_decode.js
#include "format.h"

#include <cstdio>
#include <cstring>
#include <vector>

namespace {

using namespace webdx;

int g_failures = 0;

void Expect(const char *what, const std::vector<uint8_t> &out, int texel, uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    const uint8_t *p = out.data() + 4 * texel;
    if (p[0] == r && p[1] == g && p[2] == b && p[3] == a) return;
    ++g_failures;
    printf("[dxt_decode] %s texel %d: %u %u %u %u, expected %u %u %u %u\n", what, texel, p[0], p[1], p[2], p[3], r, g, b,
           a);
}

void ColorBlock(uint8_t *b, uint16_t c0, uint16_t c1, uint32_t indices)
{
    b[0] = c0 & 0xFF, b[1] = c0 >> 8, b[2] = c1 & 0xFF, b[3] = c1 >> 8;
    for (int i = 0; i < 4; ++i) b[4 + i] = (indices >> (8 * i)) & 0xFF;
}

uint32_t Indices(const int (&index)[16])
{
    uint32_t bits = 0;
    for (int i = 0; i < 16; ++i) bits |= (uint32_t)index[i] << (2 * i);
    return bits;
}

} // namespace

int main()
{
    const int ramp[16] = {0, 1, 2, 3, 0, 1, 2, 3, 0, 1, 2, 3, 0, 1, 2, 3};
    uint8_t block[16];
    std::vector<uint8_t> out;

    ColorBlock(block, 0xF800, 0x001F, Indices(ramp));
    DecodeDxt(D3DFMT_DXT1, block, 8, 4, 4, 4, 4, out);
    Expect("DXT1 four colors", out, 0, 255, 0, 0, 255);
    Expect("DXT1 four colors", out, 1, 0, 0, 255, 255);
    Expect("DXT1 four colors", out, 2, 170, 0, 85, 255);
    Expect("DXT1 four colors", out, 3, 85, 0, 170, 255);

    out.clear();
    ColorBlock(block, 0x001F, 0xF800, Indices(ramp));
    DecodeDxt(D3DFMT_DXT1, block, 8, 4, 4, 4, 4, out);
    Expect("DXT1 three colors", out, 2, 127, 0, 127, 255);
    Expect("DXT1 three colors", out, 3, 0, 0, 0, 0);

    out.clear();
    for (int i = 0; i < 8; ++i) block[i] = (uint8_t)((2 * i) | (2 * i + 1) << 4);
    ColorBlock(block + 8, 0x001F, 0xF800, Indices(ramp));
    DecodeDxt(D3DFMT_DXT3, block, 16, 4, 4, 4, 4, out);
    Expect("DXT3", out, 0, 0, 0, 255, 0);
    Expect("DXT3", out, 7, 170, 0, 85, 119);
    Expect("DXT3", out, 15, 170, 0, 85, 255);

    const int alphaIndex[16] = {0, 1, 2, 3, 4, 5, 6, 7, 0, 0, 0, 0, 0, 0, 0, 0};
    for (int pass = 0; pass < 2; ++pass) {
        out.clear();
        block[0] = pass ? 0 : 255, block[1] = pass ? 255 : 0;
        uint64_t bits = 0;
        for (int i = 0; i < 16; ++i) bits |= (uint64_t)alphaIndex[i] << (3 * i);
        for (int i = 0; i < 6; ++i) block[2 + i] = (bits >> (8 * i)) & 0xFF;
        ColorBlock(block + 8, 0xFFFF, 0x0000, 0);
        DecodeDxt(D3DFMT_DXT5, block, 16, 4, 4, 4, 4, out);
        if (pass == 0) {
            Expect("DXT5 eight alphas", out, 1, 255, 255, 255, 0);
            Expect("DXT5 eight alphas", out, 2, 255, 255, 255, 218);
            Expect("DXT5 eight alphas", out, 7, 255, 255, 255, 36);
        } else {
            Expect("DXT5 six alphas", out, 2, 255, 255, 255, 51);
            Expect("DXT5 six alphas", out, 5, 255, 255, 255, 204);
            Expect("DXT5 six alphas", out, 6, 255, 255, 255, 0);
            Expect("DXT5 six alphas", out, 7, 255, 255, 255, 255);
        }
    }

    out.clear();
    ColorBlock(block, 0xF800, 0x001F, Indices(ramp));
    UINT rowSize = DecodeDxt(D3DFMT_DXT1, block, 8, 4, 4, 2, 2, out);
    if (rowSize != 8 || out.size() != 16) {
        ++g_failures;
        printf("[dxt_decode] 2x2 level: row size %u, %zu bytes, expected 8 and 16\n", rowSize, out.size());
    }
    Expect("2x2 level", out, 2, 255, 0, 0, 255);

    out.clear();
    uint8_t pair[16];
    ColorBlock(pair, 0xF800, 0x001F, 0);
    ColorBlock(pair + 8, 0x07E0, 0x001F, 0);
    DecodeDxt(D3DFMT_DXT1, pair, 16, 8, 4, 8, 4, out);
    Expect("second block", out, 3, 255, 0, 0, 255);
    Expect("second block", out, 4, 0, 255, 0, 255);
    Expect("second block", out, 8 * 3 + 7, 0, 255, 0, 255);

    printf("[dxt_decode] %s: %d failures\n", g_failures ? "FAILED" : "ok", g_failures);
    return g_failures ? 1 : 0;
}
