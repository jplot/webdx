// device.h — the Direct3D 9 front end: tracks state, keeps a CPU copy of every resource and records
// commands for an executor (executor.h). Its methods may be called from several threads at once.
#pragma once

#include "executor.h"
#include "gl_object.h"
#include "stream.h"

#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

namespace webdx {

class Device;
class Surface;

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
    Buffer(Device *device, UINT length, DWORD usage, D3DFORMAT format, D3DPOOL pool)
        : Resource<Iface>(device), data_(length), usage_(usage), format_(format), pool_(pool) {}
    D3DRESOURCETYPE WINAPI GetType() override;
    HRESULT WINAPI Lock(UINT offset, UINT size, void **data, DWORD flags) override;
    HRESULT WINAPI Unlock() override { return D3D_OK; }
    HRESULT WINAPI GetDesc(Desc *desc) override;

private:
    std::vector<uint8_t> data_;
    DWORD usage_;
    D3DFORMAT format_;
    D3DPOOL pool_;
};

using VertexBuffer = Buffer<IDirect3DVertexBuffer9, D3DVERTEXBUFFER_DESC>;
using IndexBuffer = Buffer<IDirect3DIndexBuffer9, D3DINDEXBUFFER_DESC>;

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
    HRESULT WINAPI UnlockRect(UINT) override { return D3D_OK; }
    HRESULT WINAPI AddDirtyRect(const RECT *) override { return D3D_OK; }

private:
    Image image_;
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
    HRESULT WINAPI UnlockRect(D3DCUBEMAP_FACES, UINT) override { return D3D_OK; }

private:
    std::vector<Image> faces_;
    DWORD usage_;
    D3DPOOL pool_;
    std::vector<Surface *> surfaces_;
};

class VolumeTexture final : public Resource<IDirect3DVolumeTexture9> {
public:
    VolumeTexture(Device *device, UINT width, UINT height, UINT depth, UINT levels, DWORD usage, D3DFORMAT format,
                  D3DPOOL pool);
    D3DRESOURCETYPE WINAPI GetType() override { return D3DRTYPE_VOLUMETEXTURE; }
    DWORD WINAPI SetLOD(DWORD) override { return 0; }
    DWORD WINAPI GetLOD() override { return 0; }
    DWORD WINAPI GetLevelCount() override { return image_.Levels(); }
    HRESULT WINAPI GetLevelDesc(UINT level, D3DVOLUME_DESC *desc) override;
    HRESULT WINAPI LockBox(UINT level, D3DLOCKED_BOX *locked, const D3DBOX *box, DWORD flags) override;
    HRESULT WINAPI UnlockBox(UINT) override { return D3D_OK; }

private:
    Image image_;
    DWORD usage_;
    D3DPOOL pool_;
};

// A render target, depth buffer or offscreen surface of its own, or one level of a texture.
class Surface final : public Resource<IDirect3DSurface9> {
public:
    Surface(Device *device, UINT width, UINT height, D3DFORMAT format, DWORD usage, D3DPOOL pool);
    Surface(Device *device, IUnknown *container, Image *image, UINT level, DWORD usage, D3DPOOL pool);
    D3DRESOURCETYPE WINAPI GetType() override { return D3DRTYPE_SURFACE; }
    HRESULT WINAPI GetContainer(REFIID, void **container) override;
    HRESULT WINAPI GetDesc(D3DSURFACE_DESC *desc) override;
    HRESULT WINAPI LockRect(D3DLOCKED_RECT *locked, const RECT *rect, DWORD flags) override;
    HRESULT WINAPI UnlockRect() override { return D3D_OK; }
    // A texture owns its level surfaces; they live as long as it does.
    ULONG WINAPI AddRef() override { return container_ ? container_->AddRef() : GLObject::AddRef(); }
    ULONG WINAPI Release() override { return container_ ? container_->Release() : GLObject::Release(); }
    void Destroy() { delete this; }

private:
    std::unique_ptr<Image> own_;
    IUnknown *container_ = nullptr;
    Image *image_;
    UINT level_;
    DWORD usage_;
    D3DPOOL pool_;
};

class VertexDeclaration final : public GLObject<IDirect3DVertexDeclaration9> {
public:
    VertexDeclaration(Device *device, const D3DVERTEXELEMENT9 *elements);
    HRESULT WINAPI GetDevice(IDirect3DDevice9 **out) override;
    HRESULT WINAPI GetDeclaration(D3DVERTEXELEMENT9 *elements, UINT *count) override;

private:
    Device *device_;
    std::vector<D3DVERTEXELEMENT9> elements_;
};

template <class Iface> class Shader final : public GLObject<Iface> {
public:
    Shader(Device *device, const DWORD *tokens);
    HRESULT WINAPI GetDevice(IDirect3DDevice9 **out) override;
    HRESULT WINAPI GetFunction(void *data, UINT *size) override;

private:
    Device *device_;
    std::vector<DWORD> tokens_;
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
    static Device *Create(const D3DPRESENT_PARAMETERS &params);
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
    HRESULT WINAPI StretchRect(IDirect3DSurface9 *, const RECT *, IDirect3DSurface9 *, const RECT *,
                               D3DTEXTUREFILTERTYPE) override
    {
        return D3D_OK;
    }

    HRESULT WINAPI SetRenderTarget(DWORD index, IDirect3DSurface9 *surface) override;
    HRESULT WINAPI SetDepthStencilSurface(IDirect3DSurface9 *surface) override;
    HRESULT WINAPI BeginScene() override { return D3D_OK; }
    HRESULT WINAPI EndScene() override { return D3D_OK; }
    HRESULT WINAPI Clear(DWORD count, const D3DRECT *rects, DWORD flags, D3DCOLOR color, float z,
                         DWORD stencil) override;
    HRESULT SetViewport(const D3DVIEWPORT9 *) { return D3D_OK; }
    HRESULT SetRenderState(D3DRENDERSTATETYPE, DWORD) { return D3D_OK; }
    HRESULT SetSamplerState(DWORD, D3DSAMPLERSTATETYPE, DWORD) { return D3D_OK; }
    HRESULT WINAPI SetTextureStageState(DWORD, D3DTEXTURESTAGESTATETYPE, DWORD) override { return D3D_OK; }
    HRESULT SetTexture(DWORD, IDirect3DBaseTexture9 *) { return D3D_OK; }
    HRESULT WINAPI SetScissorRect(const RECT *) override { return D3D_OK; }

    HRESULT SetStreamSource(UINT, IDirect3DVertexBuffer9 *, UINT, UINT) { return D3D_OK; }
    HRESULT SetIndices(IDirect3DIndexBuffer9 *) { return D3D_OK; }
    HRESULT WINAPI CreateVertexDeclaration(const D3DVERTEXELEMENT9 *elements, IDirect3DVertexDeclaration9 **out) override;
    HRESULT SetVertexDeclaration(IDirect3DVertexDeclaration9 *) { return D3D_OK; }

    HRESULT WINAPI CreateVertexShader(const DWORD *tokens, IDirect3DVertexShader9 **out) override;
    HRESULT SetVertexShader(IDirect3DVertexShader9 *) { return D3D_OK; }
    HRESULT SetVertexShaderConstantF(UINT, const float *, UINT) { return D3D_OK; }
    HRESULT WINAPI CreatePixelShader(const DWORD *tokens, IDirect3DPixelShader9 **out) override;
    HRESULT SetPixelShader(IDirect3DPixelShader9 *) { return D3D_OK; }
    HRESULT SetPixelShaderConstantF(UINT, const float *, UINT) { return D3D_OK; }

    HRESULT DrawPrimitive(D3DPRIMITIVETYPE, UINT, UINT) { return D3D_OK; }
    HRESULT DrawIndexedPrimitive(D3DPRIMITIVETYPE, INT, UINT, UINT, UINT, UINT) { return D3D_OK; }
    HRESULT WINAPI DrawPrimitiveUP(D3DPRIMITIVETYPE, UINT, const void *, UINT) override { return D3D_OK; }

    HRESULT WINAPI CreateQuery(D3DQUERYTYPE type, IDirect3DQuery9 **out) override;
    void WINAPI SetGammaRamp(UINT, DWORD, const D3DGAMMARAMP *) override {}

private:
    Device(Executor *executor, const D3DPRESENT_PARAMETERS &params);

    std::mutex mutex_;
    std::unique_ptr<Executor> executor_;
    StreamWriter stream_;
    uint32_t submitted_ = 0;
    D3DPRESENT_PARAMETERS params_;
    Surface *backBuffer_;
    Surface *depthBuffer_;
    SwapChain *swapChain_;
    IDirect3DSurface9 *renderTarget_;
};

} // namespace webdx
