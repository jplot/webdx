#include "device.h"
#include "format.h"

#include <cstring>
#include <unistd.h>

void FillDefaultCaps(D3DCAPS9 *caps);   // gl_d3d9.cpp

namespace webdx {

// ---- Image -----------------------------------------------------------------

Image::Image(D3DFORMAT format, UINT width, UINT height, UINT depth, UINT levels)
    : format_(format), width_(width), height_(height), depth_(depth)
{
    if (!levels) {
        UINT size = width > height ? width : height;
        size = size > depth ? size : depth;
        for (levels = 1; size > 1; size >>= 1) ++levels;
    }
    levels_.resize(levels);
    for (UINT i = 0; i < levels; ++i)
        levels_[i].resize((size_t)SlicePitch(i) * Depth(i));
}

UINT Image::Width(UINT level) const { return MipSize(width_, level); }
UINT Image::Height(UINT level) const { return MipSize(height_, level); }
UINT Image::Depth(UINT level) const { return MipSize(depth_, level); }
UINT Image::Pitch(UINT level) const { return RowPitch(format_, Width(level)); }
UINT Image::SlicePitch(UINT level) const { return Pitch(level) * RowCount(format_, Height(level)); }

uint8_t *Image::Bits(UINT level, const RECT *rect, UINT z)
{
    uint8_t *p = levels_[level].data() + (size_t)z * SlicePitch(level);
    if (rect) {
        FormatLayout l = LayoutOf(format_);
        p += (size_t)(rect->top / l.block) * Pitch(level) + (size_t)(rect->left / l.block) * l.bytesPerBlock;
    }
    return p;
}

// ---- Resources -------------------------------------------------------------

template <class Iface> HRESULT WINAPI Resource<Iface>::GetDevice(IDirect3DDevice9 **out)
{
    if (!out) return E_INVALIDARG;
    *out = device_;
    device_->AddRef();
    return D3D_OK;
}

template <> D3DRESOURCETYPE WINAPI VertexBuffer::GetType() { return D3DRTYPE_VERTEXBUFFER; }
template <> D3DRESOURCETYPE WINAPI IndexBuffer::GetType() { return D3DRTYPE_INDEXBUFFER; }

template <class Iface, class Desc>
HRESULT WINAPI Buffer<Iface, Desc>::Lock(UINT offset, UINT, void **data, DWORD)
{
    if (!data || offset > data_.size()) return D3DERR_INVALIDCALL;
    *data = data_.data() + offset;
    return D3D_OK;
}

template <> HRESULT WINAPI VertexBuffer::GetDesc(D3DVERTEXBUFFER_DESC *desc)
{
    if (!desc) return E_INVALIDARG;
    *desc = D3DVERTEXBUFFER_DESC{};
    desc->Format = D3DFMT_UNKNOWN;
    desc->Type = D3DRTYPE_VERTEXBUFFER;
    desc->Usage = usage_;
    desc->Pool = pool_;
    desc->Size = (UINT)data_.size();
    return D3D_OK;
}

template <> HRESULT WINAPI IndexBuffer::GetDesc(D3DINDEXBUFFER_DESC *desc)
{
    if (!desc) return E_INVALIDARG;
    *desc = D3DINDEXBUFFER_DESC{};
    desc->Format = format_;
    desc->Type = D3DRTYPE_INDEXBUFFER;
    desc->Usage = usage_;
    desc->Pool = pool_;
    desc->Size = (UINT)data_.size();
    return D3D_OK;
}

static void SurfaceDesc(const Image &image, UINT level, DWORD usage, D3DPOOL pool, D3DSURFACE_DESC *desc)
{
    *desc = D3DSURFACE_DESC{};
    desc->Format = image.Format();
    desc->Type = D3DRTYPE_SURFACE;
    desc->Usage = usage;
    desc->Pool = pool;
    desc->Width = image.Width(level);
    desc->Height = image.Height(level);
}

static void Locked(Image &image, UINT level, const RECT *rect, D3DLOCKED_RECT *locked)
{
    locked->Pitch = (INT)image.Pitch(level);
    locked->pBits = image.Bits(level, rect);
}

Texture::Texture(Device *device, UINT width, UINT height, UINT levels, DWORD usage, D3DFORMAT format, D3DPOOL pool)
    : Resource(device), image_(format, width, height, 1, levels), usage_(usage), pool_(pool)
{
    for (UINT i = 0; i < image_.Levels(); ++i)
        surfaces_.push_back(new Surface(device, this, &image_, i, usage, pool));
}

Texture::~Texture()
{
    for (Surface *s : surfaces_) s->Destroy();
}

HRESULT WINAPI Texture::GetLevelDesc(UINT level, D3DSURFACE_DESC *desc)
{
    if (!desc || level >= image_.Levels()) return D3DERR_INVALIDCALL;
    SurfaceDesc(image_, level, usage_, pool_, desc);
    return D3D_OK;
}

HRESULT WINAPI Texture::GetSurfaceLevel(UINT level, IDirect3DSurface9 **surface)
{
    if (!surface || level >= image_.Levels()) return D3DERR_INVALIDCALL;
    *surface = surfaces_[level];
    AddRef();
    return D3D_OK;
}

HRESULT WINAPI Texture::LockRect(UINT level, D3DLOCKED_RECT *locked, const RECT *rect, DWORD)
{
    if (!locked || level >= image_.Levels()) return D3DERR_INVALIDCALL;
    Locked(image_, level, rect, locked);
    return D3D_OK;
}

CubeTexture::CubeTexture(Device *device, UINT edge, UINT levels, DWORD usage, D3DFORMAT format, D3DPOOL pool)
    : Resource(device), usage_(usage), pool_(pool)
{
    for (int f = 0; f < 6; ++f) faces_.emplace_back(format, edge, edge, 1, levels);
    for (int f = 0; f < 6; ++f)
        for (UINT i = 0; i < faces_[f].Levels(); ++i)
            surfaces_.push_back(new Surface(device, this, &faces_[f], i, usage, pool));
}

CubeTexture::~CubeTexture()
{
    for (Surface *s : surfaces_) s->Destroy();
}

HRESULT WINAPI CubeTexture::GetLevelDesc(UINT level, D3DSURFACE_DESC *desc)
{
    if (!desc || level >= faces_[0].Levels()) return D3DERR_INVALIDCALL;
    SurfaceDesc(faces_[0], level, usage_, pool_, desc);
    return D3D_OK;
}

HRESULT WINAPI CubeTexture::GetCubeMapSurface(D3DCUBEMAP_FACES face, UINT level, IDirect3DSurface9 **surface)
{
    if (!surface || (UINT)face >= 6 || level >= faces_[0].Levels()) return D3DERR_INVALIDCALL;
    *surface = surfaces_[(UINT)face * faces_[0].Levels() + level];
    AddRef();
    return D3D_OK;
}

HRESULT WINAPI CubeTexture::LockRect(D3DCUBEMAP_FACES face, UINT level, D3DLOCKED_RECT *locked, const RECT *rect,
                                     DWORD)
{
    if (!locked || (UINT)face >= 6 || level >= faces_[0].Levels()) return D3DERR_INVALIDCALL;
    Locked(faces_[face], level, rect, locked);
    return D3D_OK;
}

VolumeTexture::VolumeTexture(Device *device, UINT width, UINT height, UINT depth, UINT levels, DWORD usage,
                             D3DFORMAT format, D3DPOOL pool)
    : Resource(device), image_(format, width, height, depth, levels), usage_(usage), pool_(pool)
{
}

HRESULT WINAPI VolumeTexture::GetLevelDesc(UINT level, D3DVOLUME_DESC *desc)
{
    if (!desc || level >= image_.Levels()) return D3DERR_INVALIDCALL;
    *desc = D3DVOLUME_DESC{};
    desc->Format = image_.Format();
    desc->Type = D3DRTYPE_VOLUME;
    desc->Usage = usage_;
    desc->Pool = pool_;
    desc->Width = image_.Width(level);
    desc->Height = image_.Height(level);
    desc->Depth = image_.Depth(level);
    return D3D_OK;
}

HRESULT WINAPI VolumeTexture::LockBox(UINT level, D3DLOCKED_BOX *locked, const D3DBOX *box, DWORD)
{
    if (!locked || level >= image_.Levels()) return D3DERR_INVALIDCALL;
    RECT rect = {0, 0, 0, 0};
    if (box) rect = {(LONG)box->Left, (LONG)box->Top, (LONG)box->Right, (LONG)box->Bottom};
    locked->RowPitch = (INT)image_.Pitch(level);
    locked->SlicePitch = (INT)image_.SlicePitch(level);
    locked->pBits = image_.Bits(level, box ? &rect : nullptr, box ? box->Front : 0);
    return D3D_OK;
}

Surface::Surface(Device *device, UINT width, UINT height, D3DFORMAT format, DWORD usage, D3DPOOL pool)
    : Resource(device), own_(new Image(format, width, height, 1, 1)), image_(own_.get()), level_(0), usage_(usage),
      pool_(pool)
{
}

Surface::Surface(Device *device, IUnknown *container, Image *image, UINT level, DWORD usage, D3DPOOL pool)
    : Resource(device), container_(container), image_(image), level_(level), usage_(usage), pool_(pool)
{
}

HRESULT WINAPI Surface::GetContainer(REFIID, void **container)
{
    if (!container) return E_INVALIDARG;
    if (!container_) return E_NOINTERFACE;
    *container = container_;
    container_->AddRef();
    return D3D_OK;
}

HRESULT WINAPI Surface::GetDesc(D3DSURFACE_DESC *desc)
{
    if (!desc) return E_INVALIDARG;
    SurfaceDesc(*image_, level_, usage_, pool_, desc);
    return D3D_OK;
}

HRESULT WINAPI Surface::LockRect(D3DLOCKED_RECT *locked, const RECT *rect, DWORD)
{
    if (!locked) return E_INVALIDARG;
    Locked(*image_, level_, rect, locked);
    return D3D_OK;
}

VertexDeclaration::VertexDeclaration(Device *device, const D3DVERTEXELEMENT9 *elements) : device_(device)
{
    for (; elements->Stream != 0xFF; ++elements) elements_.push_back(*elements);
    elements_.push_back(D3DVERTEXELEMENT9{0xFF, 0, D3DDECLTYPE_UNUSED, 0, 0, 0});
}

HRESULT WINAPI VertexDeclaration::GetDevice(IDirect3DDevice9 **out)
{
    if (!out) return E_INVALIDARG;
    *out = device_;
    device_->AddRef();
    return D3D_OK;
}

HRESULT WINAPI VertexDeclaration::GetDeclaration(D3DVERTEXELEMENT9 *elements, UINT *count)
{
    if (!count) return E_INVALIDARG;
    if (elements) std::memcpy(elements, elements_.data(), elements_.size() * sizeof(D3DVERTEXELEMENT9));
    *count = (UINT)elements_.size();
    return D3D_OK;
}

template <class Iface> Shader<Iface>::Shader(Device *device, const DWORD *tokens) : device_(device)
{
    size_t n = 1;
    while (tokens[n] != 0x0000FFFF) n += (tokens[n] & 0xFFFF) == 0xFFFE ? ((tokens[n] >> 16) & 0x7FFF) + 1 : 1;
    tokens_.assign(tokens, tokens + n + 1);
}

template <class Iface> HRESULT WINAPI Shader<Iface>::GetDevice(IDirect3DDevice9 **out)
{
    if (!out) return E_INVALIDARG;
    *out = device_;
    device_->AddRef();
    return D3D_OK;
}

template <class Iface> HRESULT WINAPI Shader<Iface>::GetFunction(void *data, UINT *size)
{
    if (!size) return E_INVALIDARG;
    if (data) std::memcpy(data, tokens_.data(), tokens_.size() * 4);
    *size = (UINT)tokens_.size() * 4;
    return D3D_OK;
}

template class Shader<IDirect3DVertexShader9>;
template class Shader<IDirect3DPixelShader9>;

HRESULT WINAPI Query::GetDevice(IDirect3DDevice9 **out)
{
    if (!out) return E_INVALIDARG;
    *out = device_;
    device_->AddRef();
    return D3D_OK;
}

DWORD WINAPI Query::GetDataSize()
{
    return type_ == D3DQUERYTYPE_OCCLUSION ? sizeof(DWORD) : type_ == D3DQUERYTYPE_EVENT ? sizeof(BOOL) : 0;
}

HRESULT WINAPI Query::GetData(void *data, DWORD size, DWORD)
{
    if (data && size >= sizeof(DWORD)) *(DWORD *)data = type_ == D3DQUERYTYPE_OCCLUSION ? 1024 : TRUE;
    return S_OK;
}

HRESULT WINAPI SwapChain::Present(const RECT *, const RECT *, HWND, const RGNDATA *, DWORD)
{
    return device_->Present(nullptr, nullptr, nullptr, nullptr);
}

HRESULT WINAPI SwapChain::GetBackBuffer(UINT, D3DBACKBUFFER_TYPE type, IDirect3DSurface9 **surface)
{
    return device_->GetBackBuffer(0, 0, type, surface);
}

// ---- Device ----------------------------------------------------------------

Device *Device::Create(const D3DPRESENT_PARAMETERS &params)
{
    Executor *executor = CreateWebGpuExecutor((int)params.BackBufferWidth, (int)params.BackBufferHeight);
    return executor ? new Device(executor, params) : nullptr;
}

Device::Device(Executor *executor, const D3DPRESENT_PARAMETERS &params) : executor_(executor), params_(params)
{
    backBuffer_ = new Surface(this, params.BackBufferWidth, params.BackBufferHeight, D3DFMT_A8R8G8B8,
                              D3DUSAGE_RENDERTARGET, D3DPOOL_DEFAULT);
    depthBuffer_ = new Surface(this, params.BackBufferWidth, params.BackBufferHeight, params.AutoDepthStencilFormat,
                               D3DUSAGE_DEPTHSTENCIL, D3DPOOL_DEFAULT);
    swapChain_ = new SwapChain(this);
    renderTarget_ = backBuffer_;
}

Device::~Device()
{
    backBuffer_->Release();
    depthBuffer_->Release();
    swapChain_->Release();
}

HRESULT WINAPI Device::GetDeviceCaps(D3DCAPS9 *caps)
{
    if (!caps) return E_INVALIDARG;
    FillDefaultCaps(caps);
    return D3D_OK;
}

HRESULT WINAPI Device::Reset(D3DPRESENT_PARAMETERS *params)
{
    if (params) params_ = *params;
    return D3D_OK;
}

HRESULT WINAPI Device::Present(const RECT *, const RECT *, HWND, const RGNDATA *)
{
    std::lock_guard<std::mutex> lock(mutex_);
    stream_.Put(Cmd::Present);
    executor_->Submit(stream_.Take());
    ++submitted_;
    while (submitted_ - executor_->Completed() > 2)
        usleep(500);
    return D3D_OK;
}

HRESULT WINAPI Device::GetBackBuffer(UINT, UINT, D3DBACKBUFFER_TYPE, IDirect3DSurface9 **surface)
{
    if (!surface) return E_INVALIDARG;
    *surface = backBuffer_;
    backBuffer_->AddRef();
    return D3D_OK;
}

HRESULT WINAPI Device::GetSwapChain(UINT, IDirect3DSwapChain9 **chain)
{
    if (!chain) return E_INVALIDARG;
    *chain = swapChain_;
    swapChain_->AddRef();
    return D3D_OK;
}

HRESULT WINAPI Device::CreateTexture(UINT width, UINT height, UINT levels, DWORD usage, D3DFORMAT format,
                                     D3DPOOL pool, IDirect3DTexture9 **out, HANDLE *)
{
    if (!out) return E_INVALIDARG;
    *out = new Texture(this, width, height, levels, usage, format, pool);
    return D3D_OK;
}

HRESULT WINAPI Device::CreateVolumeTexture(UINT width, UINT height, UINT depth, UINT levels, DWORD usage,
                                           D3DFORMAT format, D3DPOOL pool, IDirect3DVolumeTexture9 **out, HANDLE *)
{
    if (!out) return E_INVALIDARG;
    *out = new VolumeTexture(this, width, height, depth, levels, usage, format, pool);
    return D3D_OK;
}

HRESULT WINAPI Device::CreateCubeTexture(UINT edge, UINT levels, DWORD usage, D3DFORMAT format, D3DPOOL pool,
                                         IDirect3DCubeTexture9 **out, HANDLE *)
{
    if (!out) return E_INVALIDARG;
    *out = new CubeTexture(this, edge, levels, usage, format, pool);
    return D3D_OK;
}

HRESULT WINAPI Device::CreateVertexBuffer(UINT length, DWORD usage, DWORD, D3DPOOL pool,
                                          IDirect3DVertexBuffer9 **out, HANDLE *)
{
    if (!out) return E_INVALIDARG;
    *out = new VertexBuffer(this, length, usage, D3DFMT_UNKNOWN, pool);
    return D3D_OK;
}

HRESULT WINAPI Device::CreateIndexBuffer(UINT length, DWORD usage, D3DFORMAT format, D3DPOOL pool,
                                         IDirect3DIndexBuffer9 **out, HANDLE *)
{
    if (!out) return E_INVALIDARG;
    *out = new IndexBuffer(this, length, usage, format, pool);
    return D3D_OK;
}

HRESULT WINAPI Device::CreateRenderTarget(UINT width, UINT height, D3DFORMAT format, D3DMULTISAMPLE_TYPE, DWORD, BOOL,
                                          IDirect3DSurface9 **out, HANDLE *)
{
    if (!out) return E_INVALIDARG;
    *out = new Surface(this, width, height, format, D3DUSAGE_RENDERTARGET, D3DPOOL_DEFAULT);
    return D3D_OK;
}

HRESULT WINAPI Device::CreateDepthStencilSurface(UINT width, UINT height, D3DFORMAT format, D3DMULTISAMPLE_TYPE,
                                                 DWORD, BOOL, IDirect3DSurface9 **out, HANDLE *)
{
    if (!out) return E_INVALIDARG;
    *out = new Surface(this, width, height, format, D3DUSAGE_DEPTHSTENCIL, D3DPOOL_DEFAULT);
    return D3D_OK;
}

HRESULT WINAPI Device::CreateOffscreenPlainSurface(UINT width, UINT height, D3DFORMAT format, D3DPOOL pool,
                                                   IDirect3DSurface9 **out, HANDLE *)
{
    if (!out) return E_INVALIDARG;
    *out = new Surface(this, width, height, format, 0, pool);
    return D3D_OK;
}

HRESULT WINAPI Device::SetRenderTarget(DWORD index, IDirect3DSurface9 *surface)
{
    if (index == 0) renderTarget_ = surface;
    return D3D_OK;
}

HRESULT WINAPI Device::SetDepthStencilSurface(IDirect3DSurface9 *) { return D3D_OK; }

HRESULT WINAPI Device::Clear(DWORD, const D3DRECT *, DWORD flags, D3DCOLOR color, float, DWORD)
{
    if (!(flags & D3DCLEAR_TARGET) || renderTarget_ != backBuffer_) return D3D_OK;
    ClearCmd cmd = {{((color >> 16) & 0xFF) / 255.0f, ((color >> 8) & 0xFF) / 255.0f, (color & 0xFF) / 255.0f,
                     ((color >> 24) & 0xFF) / 255.0f}};
    std::lock_guard<std::mutex> lock(mutex_);
    stream_.Put(Cmd::Clear, cmd);
    return D3D_OK;
}

HRESULT WINAPI Device::CreateVertexDeclaration(const D3DVERTEXELEMENT9 *elements, IDirect3DVertexDeclaration9 **out)
{
    if (!out || !elements) return E_INVALIDARG;
    *out = new VertexDeclaration(this, elements);
    return D3D_OK;
}

HRESULT WINAPI Device::CreateVertexShader(const DWORD *tokens, IDirect3DVertexShader9 **out)
{
    if (!out || !tokens) return E_INVALIDARG;
    *out = new VertexShader(this, tokens);
    return D3D_OK;
}

HRESULT WINAPI Device::CreatePixelShader(const DWORD *tokens, IDirect3DPixelShader9 **out)
{
    if (!out || !tokens) return E_INVALIDARG;
    *out = new PixelShader(this, tokens);
    return D3D_OK;
}

HRESULT WINAPI Device::CreateQuery(D3DQUERYTYPE type, IDirect3DQuery9 **out)
{
    if (type != D3DQUERYTYPE_EVENT && type != D3DQUERYTYPE_OCCLUSION) return D3DERR_NOTAVAILABLE;
    if (out) *out = new Query(this, type);
    return D3D_OK;
}

} // namespace webdx
