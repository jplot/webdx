// device.h — the Direct3D 9 front end: tracks state, keeps a CPU copy of every resource and records
// commands for an executor (executor.h). Its methods may be called from several threads at once.
#pragma once

#include "executor.h"
#include "gl_object.h"
#include "sm3_wgsl.h"
#include "stream.h"

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace webdx {

class Device;

// Mip chain of one texture face, cube face or volume; level i is pitch(i) * rows(i) * depth(i) bytes.
class Image {
public:
    Image(D3DFORMAT format, UINT width, UINT height, UINT depth, UINT levels);
    D3DFORMAT Format() const { return format_; }
    UINT Width(UINT level = 0) const;
    UINT Height(UINT level = 0) const;
    UINT Depth(UINT level = 0) const;
    UINT Levels() const { return (UINT)levels_.size(); }
    UINT Pitch(UINT level) const;
    UINT SlicePitch(UINT level) const;
    uint8_t *Bits(UINT level, const RECT *rect = nullptr, UINT z = 0);

private:
    D3DFORMAT format_;
    UINT width_, height_, depth_;
    std::vector<std::vector<uint8_t>> levels_;
};

// The GPU texture behind a texture or a surface; id 0 when it has none ('NULL' render targets).
struct GpuTexture {
    uint32_t id = 0;
    TexKind kind = TexKind::Tex2D;
    TexFormat format = TexFormat::None;
};

template <class Iface> class Resource : public GLObject<Iface> {
public:
    explicit Resource(Device *device) : device_(device) {}
    HRESULT WINAPI GetDevice(IDirect3DDevice9 **out) override;
    DWORD WINAPI SetPriority(DWORD) override { return 0; }
    DWORD WINAPI GetPriority() override { return 0; }
    void WINAPI PreLoad() override {}

protected:
    Device *device_;
};

template <class Iface, class Desc> class Buffer final : public Resource<Iface> {
public:
    Buffer(Device *device, UINT length, DWORD usage, D3DFORMAT format, D3DPOOL pool);
    ~Buffer() override;
    D3DRESOURCETYPE WINAPI GetType() override;
    HRESULT WINAPI Lock(UINT offset, UINT size, void **data, DWORD flags) override;
    HRESULT WINAPI Unlock() override;
    HRESULT WINAPI GetDesc(Desc *desc) override;
    uint32_t GpuId() const { return gpu_; }
    D3DFORMAT Format() const { return format_; }

private:
    // The engine fills its dynamic buffers from two threads at once, each holding its own lock.
    struct OpenLock {
        std::thread::id thread;
        UINT offset, size;
        DWORD flags;
    };
    std::vector<uint8_t> data_;
    std::vector<OpenLock> locks_;   // guarded by the device's mutex
    UINT length_;
    uint32_t gpu_;
    DWORD usage_;
    D3DFORMAT format_;
    D3DPOOL pool_;
};

using VertexBuffer = Buffer<IDirect3DVertexBuffer9, D3DVERTEXBUFFER_DESC>;
using IndexBuffer = Buffer<IDirect3DIndexBuffer9, D3DINDEXBUFFER_DESC>;

// A render target, depth buffer or offscreen surface, or one level of one face of a texture.
class Surface final : public Resource<IDirect3DSurface9> {
public:
    Surface(Device *device, UINT width, UINT height, D3DFORMAT format, DWORD usage, D3DPOOL pool);
    Surface(Device *device, IUnknown *container, Image *image, const GpuTexture &gpu, UINT level, UINT layer,
            DWORD usage, D3DPOOL pool);
    ~Surface() override;
    D3DRESOURCETYPE WINAPI GetType() override { return D3DRTYPE_SURFACE; }
    HRESULT WINAPI GetContainer(REFIID, void **container) override;
    HRESULT WINAPI GetDesc(D3DSURFACE_DESC *desc) override;
    HRESULT WINAPI LockRect(D3DLOCKED_RECT *locked, const RECT *rect, DWORD flags) override;
    HRESULT WINAPI UnlockRect() override;
    // A texture owns its level surfaces; they live as long as it does.
    ULONG WINAPI AddRef() override { return container_ ? container_->AddRef() : GLObject::AddRef(); }
    ULONG WINAPI Release() override { return container_ ? container_->Release() : GLObject::Release(); }
    void Destroy() { delete this; }
    const GpuTexture &Gpu() const { return gpu_; }
    ViewRef View() const { return {gpu_.id, level_, layer_}; }
    UINT Width() const { return image_->Width(level_); }
    UINT Height() const { return image_->Height(level_); }

private:
    std::unique_ptr<Image> own_;
    IUnknown *container_ = nullptr;
    Image *image_;
    GpuTexture gpu_;
    UINT level_, layer_;
    DWORD usage_;
    D3DPOOL pool_;
    RECT lockRect_ = {};
    bool lockAll_ = false;
    DWORD lockFlags_ = 0;
};

class Texture final : public Resource<IDirect3DTexture9> {
public:
    Texture(Device *device, UINT width, UINT height, UINT levels, DWORD usage, D3DFORMAT format, D3DPOOL pool);
    ~Texture() override;
    D3DRESOURCETYPE WINAPI GetType() override { return D3DRTYPE_TEXTURE; }
    DWORD WINAPI SetLOD(DWORD) override { return 0; }
    DWORD WINAPI GetLOD() override { return 0; }
    DWORD WINAPI GetLevelCount() override { return image_.Levels(); }
    HRESULT WINAPI GetLevelDesc(UINT level, D3DSURFACE_DESC *desc) override;
    HRESULT WINAPI GetSurfaceLevel(UINT level, IDirect3DSurface9 **surface) override;
    HRESULT WINAPI LockRect(UINT level, D3DLOCKED_RECT *locked, const RECT *rect, DWORD flags) override;
    HRESULT WINAPI UnlockRect(UINT level) override;
    HRESULT WINAPI AddDirtyRect(const RECT *) override { return D3D_OK; }
    const GpuTexture &Gpu() const { return gpu_; }

private:
    Image image_;
    GpuTexture gpu_;
    DWORD usage_;
    D3DPOOL pool_;
    std::vector<Surface *> surfaces_;
};

class CubeTexture final : public Resource<IDirect3DCubeTexture9> {
public:
    CubeTexture(Device *device, UINT edge, UINT levels, DWORD usage, D3DFORMAT format, D3DPOOL pool);
    ~CubeTexture() override;
    D3DRESOURCETYPE WINAPI GetType() override { return D3DRTYPE_CUBETEXTURE; }
    DWORD WINAPI SetLOD(DWORD) override { return 0; }
    DWORD WINAPI GetLOD() override { return 0; }
    DWORD WINAPI GetLevelCount() override { return faces_[0].Levels(); }
    HRESULT WINAPI GetLevelDesc(UINT level, D3DSURFACE_DESC *desc) override;
    HRESULT WINAPI GetCubeMapSurface(D3DCUBEMAP_FACES face, UINT level, IDirect3DSurface9 **surface) override;
    HRESULT WINAPI LockRect(D3DCUBEMAP_FACES face, UINT level, D3DLOCKED_RECT *locked, const RECT *rect,
                            DWORD flags) override;
    HRESULT WINAPI UnlockRect(D3DCUBEMAP_FACES face, UINT level) override;
    const GpuTexture &Gpu() const { return gpu_; }

private:
    std::vector<Image> faces_;
    GpuTexture gpu_;
    DWORD usage_;
    D3DPOOL pool_;
    std::vector<Surface *> surfaces_;
};

class VolumeTexture final : public Resource<IDirect3DVolumeTexture9> {
public:
    VolumeTexture(Device *device, UINT width, UINT height, UINT depth, UINT levels, DWORD usage, D3DFORMAT format,
                  D3DPOOL pool);
    ~VolumeTexture() override;
    D3DRESOURCETYPE WINAPI GetType() override { return D3DRTYPE_VOLUMETEXTURE; }
    DWORD WINAPI SetLOD(DWORD) override { return 0; }
    DWORD WINAPI GetLOD() override { return 0; }
    DWORD WINAPI GetLevelCount() override { return image_.Levels(); }
    HRESULT WINAPI GetLevelDesc(UINT level, D3DVOLUME_DESC *desc) override;
    HRESULT WINAPI LockBox(UINT level, D3DLOCKED_BOX *locked, const D3DBOX *box, DWORD flags) override;
    HRESULT WINAPI UnlockBox(UINT level) override;
    const GpuTexture &Gpu() const { return gpu_; }

private:
    Image image_;
    GpuTexture gpu_;
    DWORD usage_;
    D3DPOOL pool_;
    D3DBOX lockBox_ = {};
    DWORD lockFlags_ = 0;
};

class VertexDeclaration final : public GLObject<IDirect3DVertexDeclaration9> {
public:
    // The declaration as vertex shader inputs (SM3AttribLocation) and the streams feeding them.
    struct Inputs {
        uint32_t attrCount = 0;
        VertexAttr attrs[kMaxAttrs];   // stream: the Direct3D stream index
        unsigned attribMask = 0, uintMask = 0, sintMask = 0, streamMask = 0;
    };

    VertexDeclaration(Device *device, const D3DVERTEXELEMENT9 *elements);
    HRESULT WINAPI GetDevice(IDirect3DDevice9 **out) override;
    HRESULT WINAPI GetDeclaration(D3DVERTEXELEMENT9 *elements, UINT *count) override;
    const Inputs &GetInputs() const { return inputs_; }

private:
    Device *device_;
    std::vector<D3DVERTEXELEMENT9> elements_;
    Inputs inputs_;
};

struct ShaderVariant {
    uint32_t module = 0;        // 0 when the translation failed
    unsigned varyingMask = 0;   // vertex shaders: the outputs written
};

template <class Iface> class Shader final : public GLObject<Iface> {
public:
    Shader(Device *device, const DWORD *tokens);
    HRESULT WINAPI GetDevice(IDirect3DDevice9 **out) override;
    HRESULT WINAPI GetFunction(void *data, UINT *size) override;
    const uint32_t *Tokens() const { return (const uint32_t *)tokens_.data(); }
    // Masks and register range, the same for every variant.
    const Sm3WgslResult &Info() const { return info_; }

    std::unordered_map<uint64_t, ShaderVariant> variants;   // by option key; guarded by the device's mutex

private:
    Device *device_;
    std::vector<DWORD> tokens_;
    Sm3WgslResult info_;
};

using VertexShader = Shader<IDirect3DVertexShader9>;
using PixelShader = Shader<IDirect3DPixelShader9>;

class Query final : public GLObject<IDirect3DQuery9> {
public:
    Query(Device *device, D3DQUERYTYPE type) : device_(device), type_(type) {}
    HRESULT WINAPI GetDevice(IDirect3DDevice9 **out) override;
    D3DQUERYTYPE WINAPI GetType() override { return type_; }
    DWORD WINAPI GetDataSize() override;
    HRESULT WINAPI Issue(DWORD) override { return D3D_OK; }
    HRESULT WINAPI GetData(void *data, DWORD size, DWORD flags) override;

private:
    Device *device_;
    D3DQUERYTYPE type_;
};

class SwapChain final : public GLObject<IDirect3DSwapChain9> {
public:
    explicit SwapChain(Device *device) : device_(device) {}
    HRESULT WINAPI Present(const RECT *, const RECT *, HWND, const RGNDATA *, DWORD) override;
    HRESULT WINAPI GetFrontBufferData(IDirect3DSurface9 *) override { return D3D_OK; }
    HRESULT WINAPI GetBackBuffer(UINT, D3DBACKBUFFER_TYPE, IDirect3DSurface9 **surface) override;

private:
    Device *device_;
};

class Device final : public GLObject<IDirect3DDevice9> {
public:
    Device(Executor *executor, const D3DPRESENT_PARAMETERS &params);
    ~Device() override;

    HRESULT WINAPI TestCooperativeLevel() override { return D3D_OK; }
    UINT WINAPI GetAvailableTextureMem() override { return 512u << 20; }
    HRESULT WINAPI GetDeviceCaps(D3DCAPS9 *caps) override;
    HRESULT WINAPI Reset(D3DPRESENT_PARAMETERS *params) override;
    HRESULT WINAPI Present(const RECT *, const RECT *, HWND, const RGNDATA *) override;
    HRESULT WINAPI GetBackBuffer(UINT, UINT, D3DBACKBUFFER_TYPE, IDirect3DSurface9 **surface) override;
    HRESULT WINAPI GetSwapChain(UINT, IDirect3DSwapChain9 **chain) override;

    HRESULT WINAPI CreateTexture(UINT width, UINT height, UINT levels, DWORD usage, D3DFORMAT format, D3DPOOL pool,
                                 IDirect3DTexture9 **out, HANDLE *) override;
    HRESULT WINAPI CreateVolumeTexture(UINT width, UINT height, UINT depth, UINT levels, DWORD usage,
                                       D3DFORMAT format, D3DPOOL pool, IDirect3DVolumeTexture9 **out,
                                       HANDLE *) override;
    HRESULT WINAPI CreateCubeTexture(UINT edge, UINT levels, DWORD usage, D3DFORMAT format, D3DPOOL pool,
                                     IDirect3DCubeTexture9 **out, HANDLE *) override;
    HRESULT WINAPI CreateVertexBuffer(UINT length, DWORD usage, DWORD fvf, D3DPOOL pool,
                                      IDirect3DVertexBuffer9 **out, HANDLE *) override;
    HRESULT WINAPI CreateIndexBuffer(UINT length, DWORD usage, D3DFORMAT format, D3DPOOL pool,
                                     IDirect3DIndexBuffer9 **out, HANDLE *) override;
    HRESULT WINAPI CreateRenderTarget(UINT width, UINT height, D3DFORMAT format, D3DMULTISAMPLE_TYPE, DWORD, BOOL,
                                      IDirect3DSurface9 **out, HANDLE *) override;
    HRESULT WINAPI CreateDepthStencilSurface(UINT width, UINT height, D3DFORMAT format, D3DMULTISAMPLE_TYPE, DWORD,
                                             BOOL, IDirect3DSurface9 **out, HANDLE *) override;
    HRESULT WINAPI CreateOffscreenPlainSurface(UINT width, UINT height, D3DFORMAT format, D3DPOOL pool,
                                               IDirect3DSurface9 **out, HANDLE *) override;

    HRESULT WINAPI UpdateSurface(IDirect3DSurface9 *, const RECT *, IDirect3DSurface9 *, const POINT *) override
    {
        return D3D_OK;
    }
    HRESULT WINAPI GetRenderTargetData(IDirect3DSurface9 *, IDirect3DSurface9 *) override { return D3D_OK; }
    HRESULT WINAPI StretchRect(IDirect3DSurface9 *src, const RECT *srcRect, IDirect3DSurface9 *dst,
                               const RECT *dstRect, D3DTEXTUREFILTERTYPE filter) override;

    HRESULT WINAPI SetRenderTarget(DWORD index, IDirect3DSurface9 *surface) override;
    HRESULT WINAPI SetDepthStencilSurface(IDirect3DSurface9 *surface) override;
    HRESULT WINAPI BeginScene() override { return D3D_OK; }
    HRESULT WINAPI EndScene() override { return D3D_OK; }
    HRESULT WINAPI Clear(DWORD count, const D3DRECT *rects, DWORD flags, D3DCOLOR color, float z,
                         DWORD stencil) override;
    HRESULT SetViewport(const D3DVIEWPORT9 *viewport);
    HRESULT SetRenderState(D3DRENDERSTATETYPE state, DWORD value);
    HRESULT SetSamplerState(DWORD sampler, D3DSAMPLERSTATETYPE type, DWORD value);
    HRESULT WINAPI SetTextureStageState(DWORD, D3DTEXTURESTAGESTATETYPE, DWORD) override { return D3D_OK; }
    HRESULT SetTexture(DWORD stage, IDirect3DBaseTexture9 *texture);
    HRESULT WINAPI SetScissorRect(const RECT *rect) override;

    HRESULT SetStreamSource(UINT stream, IDirect3DVertexBuffer9 *buffer, UINT offset, UINT stride);
    HRESULT SetIndices(IDirect3DIndexBuffer9 *buffer);
    HRESULT WINAPI CreateVertexDeclaration(const D3DVERTEXELEMENT9 *elements, IDirect3DVertexDeclaration9 **out) override;
    HRESULT SetVertexDeclaration(IDirect3DVertexDeclaration9 *declaration);

    HRESULT WINAPI CreateVertexShader(const DWORD *tokens, IDirect3DVertexShader9 **out) override;
    HRESULT SetVertexShader(IDirect3DVertexShader9 *shader);
    HRESULT SetVertexShaderConstantF(UINT start, const float *data, UINT count);
    HRESULT WINAPI CreatePixelShader(const DWORD *tokens, IDirect3DPixelShader9 **out) override;
    HRESULT SetPixelShader(IDirect3DPixelShader9 *shader);
    HRESULT SetPixelShaderConstantF(UINT start, const float *data, UINT count);

    HRESULT DrawPrimitive(D3DPRIMITIVETYPE type, UINT startVertex, UINT primCount);
    HRESULT DrawIndexedPrimitive(D3DPRIMITIVETYPE type, INT baseVertex, UINT, UINT, UINT startIndex, UINT primCount);
    HRESULT WINAPI DrawPrimitiveUP(D3DPRIMITIVETYPE, UINT, const void *, UINT) override;

    HRESULT WINAPI CreateQuery(D3DQUERYTYPE type, IDirect3DQuery9 **out) override;
    void WINAPI SetGammaRamp(UINT, DWORD, const D3DGAMMARAMP *) override {}

    std::recursive_mutex &Mutex() { return mutex_; }
    GpuTexture CreateGpuTexture(TexKind kind, D3DFORMAT format, UINT width, UINT height, UINT depth, UINT levels,
                                DWORD usage);
    void DestroyTexture(uint32_t id);
    void WriteImage(const GpuTexture &gpu, Image &image, UINT level, UINT layer, const RECT *rect, UINT front,
                    UINT back);
    uint32_t CreateBuffer(UINT size);
    void RenameBuffer(uint32_t id, UINT size);
    void WriteBuffer(uint32_t id, const uint8_t *data, UINT offset, UINT size, bool noOverwrite);
    void DestroyBuffer(uint32_t id);

private:
    struct Stream {
        VertexBuffer *buffer = nullptr;
        UINT offset = 0, stride = 0;
    };
    struct Snapshot {
        bool valid = false;
        uint32_t offset = 0, count = 0;
    };

    HRESULT Draw(D3DPRIMITIVETYPE type, UINT primCount, bool indexed, UINT first, INT baseVertex);
    void EmitTargets();
    bool ResolvePipeline(D3DPRIMITIVETYPE type, uint32_t indexFormat);
    void ResolveBindings();
    template <class S> const ShaderVariant &Variant(S *shader, uint64_t key, const Sm3WgslOptions &options);
    uint32_t Constants(Snapshot &snapshot, bool &dirty, const float (*registers)[4], int maxConst);
    uint32_t Params(float viewportWidth, float viewportHeight);
    void Flush();

    std::recursive_mutex mutex_;
    std::unique_ptr<Executor> executor_;
    StreamWriter stream_;
    uint32_t submitted_ = 0;
    uint32_t nextBuffer_ = 1, nextTexture_ = 1, nextShader_ = 1, nextPipeline_ = 1, nextBindings_ = 1;
    Surface *backBuffer_;
    Surface *depthBuffer_ = nullptr;
    SwapChain *swapChain_;

    DWORD renderStates_[256] = {};
    DWORD samplerStates_[kSamplers][14] = {};
    IDirect3DBaseTexture9 *textures_[kSamplers] = {};
    GpuTexture textureGpu_[kSamplers];
    Stream streams_[kMaxStreams];
    IndexBuffer *indices_ = nullptr;
    VertexDeclaration *declaration_ = nullptr;
    VertexShader *vs_ = nullptr;
    PixelShader *ps_ = nullptr;
    float vsConsts_[256][4] = {}, psConsts_[256][4] = {};
    D3DVIEWPORT9 viewport_ = {};
    RECT scissor_ = {};
    Surface *targets_[kMaxTargets] = {};
    Surface *depth_ = nullptr;

    bool targetsDirty_ = true, pipelineDirty_ = true, bindingsDirty_ = true;
    TargetsCmd emittedTargets_ = {};
    TexFormat targetFormats_[kMaxTargets] = {}, depthFormat_ = TexFormat::None;
    UINT targetWidth_ = 0, targetHeight_ = 0;
    PipelineCmd pipeline_ = {};
    uint32_t streamOf_[kMaxStreams] = {};   // Direct3D stream of each of the pipeline's buffers
    int vsMaxConst_ = -1, psMaxConst_ = -1;
    uint32_t bindings_ = 0;
    std::unordered_map<std::string, uint32_t> pipelines_, bindingSets_;
    std::unordered_multimap<uint32_t, std::string> bindingsByTexture_;
    Snapshot vsSnapshot_, psSnapshot_;
    bool vsConstsDirty_ = true, psConstsDirty_ = true;
    bool paramsValid_ = false;
    DrawParams params_ = {};
    uint32_t paramsOffset_ = 0;
};

} // namespace webdx
