// stream.h — the command stream the Direct3D 9 front end records and an executor replays.
// States keep their Direct3D values; each executor maps them to its API.
#pragma once

#include <cstdint>
#include <cstring>
#include <vector>

namespace webdx {

enum class Cmd : uint32_t {
    CreateBuffer,    // BufferCmd
    WriteBuffer,     // BufferWriteCmd + bytes
    RenameBuffer,    // BufferCmd: later commands see a fresh buffer (D3DLOCK_DISCARD)
    DestroyBuffer,   // BufferCmd
    CreateTexture,   // TextureCmd
    WriteTexture,    // TextureWriteCmd + bytes
    DestroyTexture,  // TextureCmd
    CreateShader,    // ShaderCmd + WGSL text
    CreatePipeline,  // PipelineCmd
    CreateBindings,  // BindingsCmd
    DestroyBindings, // BindingsCmd
    SetTargets,      // TargetsCmd
    Clear,           // ClearCmd
    Draw,            // DrawCmd
    Copy,            // CopyCmd
    Present,         // PresentCmd
};

// Formats textures take on the GPU; the front end converts Direct3D data to them.
enum class TexFormat : uint32_t {
    None, BGRA8, RGBA8, RGBA16F, RG32F, R32F, BC1, BC2, BC3, Depth24S8, Depth32FS8,
};

inline bool IsDepth(TexFormat f) { return f == TexFormat::Depth24S8 || f == TexFormat::Depth32FS8; }

enum class TexKind : uint32_t { Tex2D, Cube, Tex3D };

enum : uint32_t {
    kMaxTargets = 4, kMaxStreams = 4, kMaxAttrs = 16,
    kPixelSamplers = 16, kVertexSamplers = 4, kSamplers = kPixelSamplers + kVertexSamplers,
};

// Texture type a shader declares per sampler: 0 unused, then the sm3 dimension codes, then depth comparison.
enum : uint8_t { kSlot2D = 2, kSlotCube = 3, kSlot3D = 4, kSlotDepth = 5 };

struct BufferCmd {
    uint32_t id, size;
};

struct BufferWriteCmd {
    uint32_t id, offset, noOverwrite;
};

struct TextureCmd {
    uint32_t id;
    TexKind kind;
    TexFormat format;
    uint32_t width, height, depth, levels, renderable;
};

struct TextureWriteCmd {
    uint32_t id, level, x, y, z, width, height, depth, bytesPerRow, rowsPerImage;
};

struct ShaderCmd {
    uint32_t id;
};

// One level of one cube face or array layer; texture 0 = none.
struct ViewRef {
    uint32_t texture, level, layer;
};

struct VertexAttr {
    uint32_t stream, type, offset, location;   // type: D3DDECLTYPE; stream: index into the draw's buffers
};

struct PipelineCmd {
    uint32_t id, vs, ps;
    uint32_t topology;      // D3DPRIMITIVETYPE
    uint32_t indexFormat;   // 16 or 32 for indexed strips, else 0
    uint32_t streamCount, strides[kMaxStreams];
    uint32_t attrCount;
    VertexAttr attrs[kMaxAttrs];
    TexFormat colorFormats[kMaxTargets];
    TexFormat depthFormat;
    uint32_t writeMasks[kMaxTargets];
    uint32_t blendEnable, srcBlend, dstBlend, blendOp, srcBlendAlpha, dstBlendAlpha, blendOpAlpha;
    uint32_t zEnable, zWrite, zFunc;
    float depthBias, slopeBias;
    uint32_t cullMode;
    uint32_t stencilEnable, stencilMask, stencilWriteMask;
    uint32_t stencilFunc, stencilFail, stencilZFail, stencilPass;
    uint32_t ccwFunc, ccwFail, ccwZFail, ccwPass;
    uint8_t slots[kSamplers];
};

struct SamplerDesc {
    uint32_t addressU, addressV, addressW, magFilter, minFilter, mipFilter, maxAnisotropy;
};

struct BindingsCmd {
    uint32_t id;
    uint8_t slots[kSamplers];
    uint32_t textures[kSamplers];   // 0: the executor's placeholder of the slot's type
    SamplerDesc samplers[kSamplers];
};

struct TargetsCmd {
    ViewRef colors[kMaxTargets];
    ViewRef depth;
    uint32_t width, height;
};

struct ClearCmd {
    uint32_t flags;   // D3DCLEAR_*
    float color[4];
    float depth;
    uint32_t stencil;
};

struct DrawCmd {
    uint32_t pipeline, bindings;   // bindings 0: the pipeline samples nothing
    float viewport[6];             // x, y, width, height, minZ, maxZ
    int32_t scissor[4];            // x, y, width, height
    uint32_t stencilRef, blendFactor;
    uint32_t vertexBuffers[kMaxStreams], vertexOffsets[kMaxStreams];
    uint32_t indexBuffer, indexFormat;   // indexBuffer 0: not indexed
    uint32_t uniforms[3];          // byte offsets of the vertex constants, pixel constants, pixel parameters
    uint32_t count, first;
    int32_t baseVertex;
};

// Blits take a BlitParams block in the uniforms: the source rectangle in normalized coordinates.
struct BlitParams {
    float u0, v0, u1, v1;
};

struct CopyCmd {
    ViewRef src, dst;
    int32_t dstRect[4];   // x, y, width, height
    uint32_t linear, params;
};

struct PresentCmd {
    uint32_t backBuffer, params;
};

// Draws address their constants by byte offset in the uniforms sent with the commands; each block
// starts on kUniformAlign and the shaders bind kConstantBlock bytes from it.
constexpr uint32_t kUniformAlign = 256;
constexpr uint32_t kConstantBlock = 256 * 16;

struct PixelParams {
    int32_t alphaFunc;   // 0 when the alpha test is off
    float alphaRef;
    float pad[2];
};

class StreamWriter {
public:
    template <class T> void Put(Cmd cmd, const T &payload, const void *extra = nullptr, uint32_t extraSize = 0)
    {
        uint32_t h[2] = {(uint32_t)cmd, (uint32_t)sizeof(T) + extraSize};
        Append(h, sizeof h);
        Append(&payload, sizeof(T));
        if (extraSize) Append(extra, extraSize);
    }
    uint32_t Uniform(const void *data, uint32_t size)
    {
        uint32_t at = (uint32_t)uniforms_.size();
        uniforms_.resize(at + (size + kUniformAlign - 1) / kUniformAlign * kUniformAlign);
        std::memcpy(uniforms_.data() + at, data, size);
        return at;
    }
    std::vector<uint8_t> TakeCommands()
    {
        std::vector<uint8_t> out;
        out.swap(commands_);
        return out;
    }
    std::vector<uint8_t> TakeUniforms()
    {
        std::vector<uint8_t> out;
        out.swap(uniforms_);
        if (!out.empty()) out.resize(out.size() + kConstantBlock);
        return out;
    }
    size_t Size() const { return commands_.size() + uniforms_.size(); }

private:
    void Append(const void *p, size_t n)
    {
        size_t at = commands_.size();
        commands_.resize(at + n);
        std::memcpy(commands_.data() + at, p, n);
    }
    std::vector<uint8_t> commands_;
    std::vector<uint8_t> uniforms_;
};

class StreamReader {
public:
    StreamReader(const uint8_t *p, size_t n) : p_(p), end_(p + n) {}
    bool Next(Cmd &cmd, const uint8_t *&payload, uint32_t &size)
    {
        if (p_ + 8 > end_) return false;
        uint32_t h[2];
        std::memcpy(h, p_, sizeof h);
        cmd = (Cmd)h[0];
        size = h[1];
        payload = p_ + 8;
        p_ += 8 + size;
        return true;
    }
    template <class T> static T Read(const uint8_t *payload)
    {
        T t;
        std::memcpy(&t, payload, sizeof t);
        return t;
    }

private:
    const uint8_t *p_;
    const uint8_t *end_;
};

} // namespace webdx
