// sm3_wgsl.h — Direct3D 9 shader model 3 bytecode (vs_3_0 / ps_3_0) to WGSL.
//
// Binding contract with the WebGPU device:
//   group 0: binding 0 vertex constants, 1 pixel constants (array<vec4f, 256>),
//            2 pixel parameters (alpha test), 3 vertex / 4 pixel integer constants
//   group 1: pixel sampler s -> texture 2s, sampler 2s + 1; vertex sampler s -> 32 + 2s, 33 + 2s
//   vertex inputs: SM3AttribLocation(usage, index); varyings: TEXCOORD0-9 at 0-9, COLOR0-1 at 10-11
#pragma once

#include <cstdint>
#include <string>

struct Sm3WgslOptions {
    unsigned shadowMask = 0;   // pixel samplers bound to depth textures: comparison sampling
    unsigned varyingMask = ~0u; // pixel inputs the vertex stage writes (bit = location); others read 0
};

struct Sm3WgslResult {
    bool ok = false;
    bool pixel = false;
    std::string wgsl;
    std::string error;
    unsigned samplerMask = 0;  // sampler registers the shader reads
    unsigned sampler2DMask = 0; // of those, the 2D ones (the only ones that can take a depth texture)
    unsigned varyingMask = 0;  // vertex: locations written; pixel: locations read
    int maxConst = -1;         // highest float constant register read (255 with relative addressing)
};

int SM3AttribLocation(int usage, int index);
Sm3WgslResult TranslateSm3ToWgsl(const uint32_t *tokens, const Sm3WgslOptions &options);
