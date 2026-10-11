#include "device.h"
#include "format.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <unistd.h>

void FillDefaultCaps(D3DCAPS9 *caps);   // gl_d3d9.cpp

namespace webdx {

namespace {

constexpr DWORD kVertexSampler0 = 257;   // D3DVERTEXTEXTURESAMPLER0
constexpr D3DRENDERSTATETYPE kBlendFactor = (D3DRENDERSTATETYPE)193;
// Past this many recorded bytes (texture uploads while loading), the stream goes out before Present.
constexpr size_t kFlushBytes = 32u << 20;

template <class T> void Rebind(T *&slot, T *value)
{
    if (value) value->AddRef();
    if (slot) slot->Release();
    slot = value;
}

float AsFloat(DWORD bits)
{
    float f;
    std::memcpy(&f, &bits, 4);
    return f;
}

void WarnOnce(bool &warned, const char *message)
{
    if (warned) return;
    warned = true;
    fprintf(stderr, "[webdx] %s\n", message);
}

GpuTexture GpuOf(IDirect3DBaseTexture9 *texture)
{
    switch (texture->GetType()) {
    case D3DRTYPE_TEXTURE: return static_cast<Texture *>(static_cast<IDirect3DTexture9 *>(texture))->Gpu();
    case D3DRTYPE_CUBETEXTURE:
        return static_cast<CubeTexture *>(static_cast<IDirect3DCubeTexture9 *>(texture))->Gpu();
    case D3DRTYPE_VOLUMETEXTURE:
        return static_cast<VolumeTexture *>(static_cast<IDirect3DVolumeTexture9 *>(texture))->Gpu();
    default: return {};
    }
}

bool PipelineState(DWORD state)
{
    switch (state) {
    case D3DRS_ZENABLE: case D3DRS_ZWRITEENABLE: case D3DRS_SRCBLEND: case D3DRS_DESTBLEND: case D3DRS_CULLMODE:
    case D3DRS_ZFUNC: case D3DRS_ALPHABLENDENABLE: case D3DRS_STENCILENABLE: case D3DRS_STENCILFAIL:
    case D3DRS_STENCILZFAIL: case D3DRS_STENCILPASS: case D3DRS_STENCILFUNC: case D3DRS_STENCILMASK:
    case D3DRS_STENCILWRITEMASK: case D3DRS_COLORWRITEENABLE: case D3DRS_BLENDOP: case D3DRS_SLOPESCALEDEPTHBIAS:
    case D3DRS_TWOSIDEDSTENCILMODE: case D3DRS_CCW_STENCILFAIL: case D3DRS_CCW_STENCILZFAIL:
    case D3DRS_CCW_STENCILPASS: case D3DRS_CCW_STENCILFUNC: case D3DRS_COLORWRITEENABLE1:
    case D3DRS_COLORWRITEENABLE2: case D3DRS_COLORWRITEENABLE3: case D3DRS_DEPTHBIAS:
    case D3DRS_SEPARATEALPHABLENDENABLE: case D3DRS_SRCBLENDALPHA: case D3DRS_DESTBLENDALPHA: case D3DRS_BLENDOPALPHA:
        return true;
    default: return false;
    }
}

} // namespace

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

template <class Iface, class Desc>
Buffer<Iface, Desc>::Buffer(Device *device, UINT length, DWORD usage, D3DFORMAT format, D3DPOOL pool)
    : Resource<Iface>(device), data_((length + 3) & ~3u), length_(length), usage_(usage), format_(format), pool_(pool)
{
    gpu_ = device->CreateBuffer((UINT)data_.size());
}

template <class Iface, class Desc> Buffer<Iface, Desc>::~Buffer() { this->device_->DestroyBuffer(gpu_); }

template <> D3DRESOURCETYPE WINAPI VertexBuffer::GetType() { return D3DRTYPE_VERTEXBUFFER; }
template <> D3DRESOURCETYPE WINAPI IndexBuffer::GetType() { return D3DRTYPE_INDEXBUFFER; }

template <class Iface, class Desc>
HRESULT WINAPI Buffer<Iface, Desc>::Lock(UINT offset, UINT size, void **data, DWORD flags)
{
    if (!data || offset > length_) return D3DERR_INVALIDCALL;
    if (!size || size > length_ - offset) size = length_ - offset;
    std::lock_guard<std::recursive_mutex> lock(this->device_->Mutex());
    if (flags & D3DLOCK_DISCARD) this->device_->RenameBuffer(gpu_, (UINT)data_.size());
    locks_.push_back({std::this_thread::get_id(), offset, size, flags});
    *data = data_.data() + offset;
    return D3D_OK;
}

template <class Iface, class Desc> HRESULT WINAPI Buffer<Iface, Desc>::Unlock()
{
    std::lock_guard<std::recursive_mutex> lock(this->device_->Mutex());
    std::thread::id self = std::this_thread::get_id();
    for (size_t i = locks_.size(); i-- > 0;) {
        if (locks_[i].thread != self) continue;
        OpenLock open = locks_[i];
        locks_.erase(locks_.begin() + i);
        if (!(open.flags & D3DLOCK_READONLY)) {
            UINT begin = open.offset & ~3u, end = (open.offset + open.size + 3) & ~3u;
            this->device_->WriteBuffer(gpu_, data_.data() + begin, begin, end - begin,
                                       open.flags & D3DLOCK_NOOVERWRITE);
        }
        return D3D_OK;
    }
    return D3DERR_INVALIDCALL;
}

template <> HRESULT WINAPI VertexBuffer::GetDesc(D3DVERTEXBUFFER_DESC *desc)
{
    if (!desc) return E_INVALIDARG;
    *desc = D3DVERTEXBUFFER_DESC{};
    desc->Format = D3DFMT_UNKNOWN;
    desc->Type = D3DRTYPE_VERTEXBUFFER;
    desc->Usage = usage_;
    desc->Pool = pool_;
    desc->Size = length_;
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
    desc->Size = length_;
    return D3D_OK;
}

template class Buffer<IDirect3DVertexBuffer9, D3DVERTEXBUFFER_DESC>;
template class Buffer<IDirect3DIndexBuffer9, D3DINDEXBUFFER_DESC>;

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

Texture::Texture(Device *device, UINT width, UINT height, UINT levels, DWORD usage, D3DFORMAT format, D3DPOOL pool)
    : Resource(device), image_(format, width, height, 1, levels), usage_(usage), pool_(pool)
{
    gpu_ = device->CreateGpuTexture(TexKind::Tex2D, format, width, height, 1, image_.Levels(), usage);
    for (UINT i = 0; i < image_.Levels(); ++i)
        surfaces_.push_back(new Surface(device, this, &image_, gpu_, i, 0, usage, pool));
}

Texture::~Texture()
{
    for (Surface *s : surfaces_) s->Destroy();
    device_->DestroyTexture(gpu_.id);
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

HRESULT WINAPI Texture::LockRect(UINT level, D3DLOCKED_RECT *locked, const RECT *rect, DWORD flags)
{
    if (level >= image_.Levels()) return D3DERR_INVALIDCALL;
    return surfaces_[level]->LockRect(locked, rect, flags);
}

HRESULT WINAPI Texture::UnlockRect(UINT level)
{
    if (level >= image_.Levels()) return D3DERR_INVALIDCALL;
    return surfaces_[level]->UnlockRect();
}

CubeTexture::CubeTexture(Device *device, UINT edge, UINT levels, DWORD usage, D3DFORMAT format, D3DPOOL pool)
    : Resource(device), usage_(usage), pool_(pool)
{
    for (int f = 0; f < 6; ++f) faces_.emplace_back(format, edge, edge, 1, levels);
    gpu_ = device->CreateGpuTexture(TexKind::Cube, format, edge, edge, 1, faces_[0].Levels(), usage);
    for (int f = 0; f < 6; ++f)
        for (UINT i = 0; i < faces_[f].Levels(); ++i)
            surfaces_.push_back(new Surface(device, this, &faces_[f], gpu_, i, f, usage, pool));
}

CubeTexture::~CubeTexture()
{
    for (Surface *s : surfaces_) s->Destroy();
    device_->DestroyTexture(gpu_.id);
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
                                     DWORD flags)
{
    if ((UINT)face >= 6 || level >= faces_[0].Levels()) return D3DERR_INVALIDCALL;
    return surfaces_[(UINT)face * faces_[0].Levels() + level]->LockRect(locked, rect, flags);
}

HRESULT WINAPI CubeTexture::UnlockRect(D3DCUBEMAP_FACES face, UINT level)
{
    if ((UINT)face >= 6 || level >= faces_[0].Levels()) return D3DERR_INVALIDCALL;
    return surfaces_[(UINT)face * faces_[0].Levels() + level]->UnlockRect();
}

VolumeTexture::VolumeTexture(Device *device, UINT width, UINT height, UINT depth, UINT levels, DWORD usage,
                             D3DFORMAT format, D3DPOOL pool)
    : Resource(device), image_(format, width, height, depth, levels), usage_(usage), pool_(pool)
{
    gpu_ = device->CreateGpuTexture(TexKind::Tex3D, format, width, height, depth, image_.Levels(), usage);
}

VolumeTexture::~VolumeTexture() { device_->DestroyTexture(gpu_.id); }

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

HRESULT WINAPI VolumeTexture::LockBox(UINT level, D3DLOCKED_BOX *locked, const D3DBOX *box, DWORD flags)
{
    if (!locked || level >= image_.Levels()) return D3DERR_INVALIDCALL;
    lockBox_ = box ? *box : D3DBOX{0, 0, image_.Width(level), image_.Height(level), 0, image_.Depth(level)};
    lockFlags_ = flags;
    RECT rect = {(LONG)lockBox_.Left, (LONG)lockBox_.Top, (LONG)lockBox_.Right, (LONG)lockBox_.Bottom};
    locked->RowPitch = (INT)image_.Pitch(level);
    locked->SlicePitch = (INT)image_.SlicePitch(level);
    locked->pBits = image_.Bits(level, &rect, lockBox_.Front);
    return D3D_OK;
}

HRESULT WINAPI VolumeTexture::UnlockBox(UINT level)
{
    if (level >= image_.Levels()) return D3DERR_INVALIDCALL;
    if (lockFlags_ & D3DLOCK_READONLY) return D3D_OK;
    RECT rect = {(LONG)lockBox_.Left, (LONG)lockBox_.Top, (LONG)lockBox_.Right, (LONG)lockBox_.Bottom};
    device_->WriteImage(gpu_, image_, level, 0, &rect, lockBox_.Front, lockBox_.Back);
    return D3D_OK;
}

Surface::Surface(Device *device, UINT width, UINT height, D3DFORMAT format, DWORD usage, D3DPOOL pool)
    : Resource(device), own_(new Image(format, width, height, 1, 1)), image_(own_.get()), level_(0), layer_(0),
      usage_(usage), pool_(pool)
{
    gpu_ = device->CreateGpuTexture(TexKind::Tex2D, format, width, height, 1, 1, usage);
}

Surface::Surface(Device *device, IUnknown *container, Image *image, const GpuTexture &gpu, UINT level, UINT layer,
                 DWORD usage, D3DPOOL pool)
    : Resource(device), container_(container), image_(image), gpu_(gpu), level_(level), layer_(layer), usage_(usage),
      pool_(pool)
{
}

Surface::~Surface()
{
    if (!container_) device_->DestroyTexture(gpu_.id);
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

HRESULT WINAPI Surface::LockRect(D3DLOCKED_RECT *locked, const RECT *rect, DWORD flags)
{
    if (!locked) return E_INVALIDARG;
    lockAll_ = !rect;
    if (rect) lockRect_ = *rect;
    lockFlags_ = flags;
    locked->Pitch = (INT)image_->Pitch(level_);
    locked->pBits = image_->Bits(level_, rect);
    return D3D_OK;
}

HRESULT WINAPI Surface::UnlockRect()
{
    if (!(lockFlags_ & D3DLOCK_READONLY))
        device_->WriteImage(gpu_, *image_, level_, layer_, lockAll_ ? nullptr : &lockRect_, 0, 1);
    return D3D_OK;
}

VertexDeclaration::VertexDeclaration(Device *device, const D3DVERTEXELEMENT9 *elements) : device_(device)
{
    for (; elements->Stream != 0xFF; ++elements) {
        elements_.push_back(*elements);
        int location = SM3AttribLocation(elements->Usage, elements->UsageIndex);
        if (location < 0 || elements->Stream >= kMaxStreams || (inputs_.attribMask >> location & 1)) continue;
        inputs_.attrs[inputs_.attrCount++] = {elements->Stream, elements->Type, elements->Offset, (uint32_t)location};
        inputs_.attribMask |= 1u << location;
        inputs_.streamMask |= 1u << elements->Stream;
        if (elements->Type == D3DDECLTYPE_UBYTE4) inputs_.uintMask |= 1u << location;
        if (elements->Type == D3DDECLTYPE_SHORT2 || elements->Type == D3DDECLTYPE_SHORT4)
            inputs_.sintMask |= 1u << location;
    }
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
    info_ = TranslateSm3ToWgsl(Tokens(), Sm3WgslOptions{});
    info_.wgsl.clear();
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

// ---- Device: resources -----------------------------------------------------

Device::Device(Executor *executor, const D3DPRESENT_PARAMETERS &params)
    : executor_(executor), features_(executor->GetFeatures())
{
    DWORD *rs = renderStates_;
    rs[D3DRS_ZENABLE] = params.EnableAutoDepthStencil ? D3DZB_TRUE : D3DZB_FALSE;
    rs[D3DRS_ZWRITEENABLE] = TRUE;
    rs[D3DRS_SRCBLEND] = rs[D3DRS_SRCBLENDALPHA] = D3DBLEND_ONE;
    rs[D3DRS_DESTBLEND] = rs[D3DRS_DESTBLENDALPHA] = D3DBLEND_ZERO;
    rs[D3DRS_CULLMODE] = D3DCULL_CCW;
    rs[D3DRS_ZFUNC] = D3DCMP_LESSEQUAL;
    rs[D3DRS_ALPHAFUNC] = rs[D3DRS_STENCILFUNC] = rs[D3DRS_CCW_STENCILFUNC] = D3DCMP_ALWAYS;
    rs[D3DRS_STENCILFAIL] = rs[D3DRS_STENCILZFAIL] = rs[D3DRS_STENCILPASS] = D3DSTENCILOP_KEEP;
    rs[D3DRS_CCW_STENCILFAIL] = rs[D3DRS_CCW_STENCILZFAIL] = rs[D3DRS_CCW_STENCILPASS] = D3DSTENCILOP_KEEP;
    rs[D3DRS_STENCILMASK] = rs[D3DRS_STENCILWRITEMASK] = 0xFFFFFFFF;
    rs[D3DRS_COLORWRITEENABLE] = rs[D3DRS_COLORWRITEENABLE1] = 0xF;
    rs[D3DRS_COLORWRITEENABLE2] = rs[D3DRS_COLORWRITEENABLE3] = 0xF;
    rs[D3DRS_BLENDOP] = rs[D3DRS_BLENDOPALPHA] = D3DBLENDOP_ADD;
    rs[kBlendFactor] = 0xFFFFFFFF;
    for (auto &s : samplerStates_) {
        s[D3DSAMP_ADDRESSU] = s[D3DSAMP_ADDRESSV] = s[D3DSAMP_ADDRESSW] = 1;   // D3DTADDRESS_WRAP
        s[D3DSAMP_MAGFILTER] = s[D3DSAMP_MINFILTER] = 1;                        // D3DTEXF_POINT
        s[D3DSAMP_MAXANISOTROPY] = 1;
    }

    backBuffer_ = new Surface(this, params.BackBufferWidth, params.BackBufferHeight, D3DFMT_A8R8G8B8,
                              D3DUSAGE_RENDERTARGET, D3DPOOL_DEFAULT);
    Rebind(targets_[0], backBuffer_);
    if (params.EnableAutoDepthStencil) {
        depthBuffer_ = new Surface(this, params.BackBufferWidth, params.BackBufferHeight,
                                   params.AutoDepthStencilFormat, D3DUSAGE_DEPTHSTENCIL, D3DPOOL_DEFAULT);
        Rebind(depth_, depthBuffer_);
    }
    swapChain_ = new SwapChain(this);
    viewport_ = {0, 0, params.BackBufferWidth, params.BackBufferHeight, 0.0f, 1.0f};
    scissor_ = {0, 0, (LONG)params.BackBufferWidth, (LONG)params.BackBufferHeight};
}

Device::~Device()
{
    for (auto &t : textures_) Rebind(t, (IDirect3DBaseTexture9 *)nullptr);
    for (auto &s : streams_) Rebind(s.buffer, (VertexBuffer *)nullptr);
    Rebind(indices_, (IndexBuffer *)nullptr);
    Rebind(declaration_, (VertexDeclaration *)nullptr);
    Rebind(vs_, (VertexShader *)nullptr);
    Rebind(ps_, (PixelShader *)nullptr);
    for (auto &t : targets_) Rebind(t, (Surface *)nullptr);
    Rebind(depth_, (Surface *)nullptr);
    backBuffer_->Release();
    if (depthBuffer_) depthBuffer_->Release();
    swapChain_->Release();
}

GpuTexture Device::CreateGpuTexture(TexKind kind, D3DFORMAT format, UINT width, UINT height, UINT depth,
                                    UINT levels, DWORD usage)
{
    GpuTexture gpu;
    gpu.kind = kind;
    gpu.format = GpuFormatOf(format, features_);
    if (gpu.format == TexFormat::None) return gpu;
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    gpu.id = nextTexture_++;
    TextureCmd cmd = {gpu.id, kind, gpu.format, width, height, depth, levels,
                      (usage & (D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL)) ? 1u : 0u};
    stream_.Put(Cmd::CreateTexture, cmd);
    return gpu;
}

void Device::DestroyTexture(uint32_t id)
{
    if (!id) return;
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    auto range = bindingsByTexture_.equal_range(id);
    for (auto it = range.first; it != range.second; ++it) {
        auto set = bindingSets_.find(it->second);
        if (set == bindingSets_.end()) continue;
        if (set->second == bindings_) bindingsDirty_ = true;
        BindingsCmd cmd;
        std::memset(&cmd, 0, sizeof cmd);
        cmd.id = set->second;
        stream_.Put(Cmd::DestroyBindings, cmd);
        bindingSets_.erase(set);
    }
    bindingsByTexture_.erase(id);
    TextureCmd cmd = {};
    cmd.id = id;
    stream_.Put(Cmd::DestroyTexture, cmd);
}

void Device::WriteImage(const GpuTexture &gpu, Image &image, UINT level, UINT layer, const RECT *rect, UINT front,
                        UINT back)
{
    static bool unconverted;
    if (!gpu.id || IsDepth(gpu.format)) return;
    D3DFORMAT format = image.Format();
    UINT block = LayoutOf(format).block;
    bool decode = block > 1 && gpu.format == TexFormat::RGBA8;
    // Compressed copies cover whole blocks, up to the level's size rounded up to the block.
    UINT width = (image.Width(level) + block - 1) / block * block;
    UINT height = (image.Height(level) + block - 1) / block * block;
    RECT r = rect ? *rect : RECT{0, 0, (LONG)width, (LONG)height};
    UINT x = (UINT)r.left / block * block, y = (UINT)r.top / block * block;
    UINT x1 = std::min(width, ((UINT)r.right + block - 1) / block * block);
    UINT y1 = std::min(height, ((UINT)r.bottom + block - 1) / block * block);
    if (x1 <= x || y1 <= y || back <= front) return;
    UINT outWidth = decode ? std::min(x1, image.Width(level)) - x : x1 - x;
    UINT outHeight = decode ? std::min(y1, image.Height(level)) - y : y1 - y;
    TextureWriteCmd cmd = {gpu.id, level, x, y, layer + front, outWidth, outHeight, back - front, 0,
                           decode ? outHeight : outHeight / block};
    std::vector<uint8_t> bytes;
    RECT at = {(LONG)x, (LONG)y, (LONG)x1, (LONG)y1};
    for (UINT z = front; z < back; ++z) {
        const uint8_t *src = image.Bits(level, &at, z);
        cmd.bytesPerRow = decode ? DecodeDxt(format, src, image.Pitch(level), x1 - x, y1 - y, outWidth, outHeight, bytes)
                                 : ConvertRows(format, src, image.Pitch(level), x1 - x, cmd.rowsPerImage, bytes);
    }
    if (!cmd.bytesPerRow) return WarnOnce(unconverted, "texture upload in a format without conversion");
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    stream_.Put(Cmd::WriteTexture, cmd, bytes.data(), (uint32_t)bytes.size());
    if (stream_.Size() > kFlushBytes) Flush();
}

uint32_t Device::CreateBuffer(UINT size)
{
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    uint32_t id = nextBuffer_++;
    stream_.Put(Cmd::CreateBuffer, BufferCmd{id, size});
    return id;
}

void Device::RenameBuffer(uint32_t id, UINT size)
{
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    stream_.Put(Cmd::RenameBuffer, BufferCmd{id, size});
}

void Device::WriteBuffer(uint32_t id, const uint8_t *data, UINT offset, UINT size, bool noOverwrite)
{
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    stream_.Put(Cmd::WriteBuffer, BufferWriteCmd{id, offset, noOverwrite ? 1u : 0u}, data, size);
    if (stream_.Size() > kFlushBytes) Flush();
}

void Device::DestroyBuffer(uint32_t id)
{
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    stream_.Put(Cmd::DestroyBuffer, BufferCmd{id, 0});
}

HRESULT WINAPI Device::GetDeviceCaps(D3DCAPS9 *caps)
{
    if (!caps) return E_INVALIDARG;
    FillDefaultCaps(caps);
    return D3D_OK;
}

HRESULT WINAPI Device::Reset(D3DPRESENT_PARAMETERS *) { return D3D_OK; }

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

// ---- Device: state ---------------------------------------------------------

HRESULT WINAPI Device::SetRenderTarget(DWORD index, IDirect3DSurface9 *surface)
{
    if (index >= kMaxTargets || (index == 0 && !surface)) return D3DERR_INVALIDCALL;
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    Rebind(targets_[index], static_cast<Surface *>(surface));
    if (index == 0) {
        Surface *s = targets_[0];
        viewport_ = {0, 0, s->Width(), s->Height(), 0.0f, 1.0f};
        scissor_ = {0, 0, (LONG)s->Width(), (LONG)s->Height()};
    }
    targetsDirty_ = true;
    return D3D_OK;
}

HRESULT WINAPI Device::SetDepthStencilSurface(IDirect3DSurface9 *surface)
{
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    Rebind(depth_, static_cast<Surface *>(surface));
    targetsDirty_ = true;
    return D3D_OK;
}

HRESULT Device::SetViewport(const D3DVIEWPORT9 *viewport)
{
    if (!viewport) return D3DERR_INVALIDCALL;
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    viewport_ = *viewport;
    return D3D_OK;
}

HRESULT WINAPI Device::SetScissorRect(const RECT *rect)
{
    if (!rect) return D3DERR_INVALIDCALL;
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    scissor_ = *rect;
    return D3D_OK;
}

HRESULT Device::SetRenderState(D3DRENDERSTATETYPE state, DWORD value)
{
    if ((DWORD)state >= 256) return D3DERR_INVALIDCALL;
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (renderStates_[state] == value) return D3D_OK;
    renderStates_[state] = value;
    if (PipelineState(state)) pipelineDirty_ = true;
    return D3D_OK;
}

HRESULT Device::SetSamplerState(DWORD sampler, D3DSAMPLERSTATETYPE type, DWORD value)
{
    DWORD s = sampler >= kVertexSampler0 ? kPixelSamplers + sampler - kVertexSampler0 : sampler;
    if (s >= kSamplers || (DWORD)type >= 14) return D3DERR_INVALIDCALL;
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (samplerStates_[s][type] == value) return D3D_OK;
    samplerStates_[s][type] = value;
    if (pipeline_.slots[s]) bindingsDirty_ = true;
    return D3D_OK;
}

HRESULT Device::SetTexture(DWORD stage, IDirect3DBaseTexture9 *texture)
{
    DWORD s = stage >= kVertexSampler0 ? kPixelSamplers + stage - kVertexSampler0 : stage;
    if (s >= kSamplers) return D3DERR_INVALIDCALL;
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (textures_[s] == texture) return D3D_OK;
    GpuTexture gpu = texture ? GpuOf(texture) : GpuTexture{};
    // Depth textures sample with comparison: another pixel shader variant.
    if (IsDepth(gpu.format) != IsDepth(textureGpu_[s].format)) pipelineDirty_ = true;
    Rebind(textures_[s], texture);
    textureGpu_[s] = gpu;
    if (pipeline_.slots[s]) bindingsDirty_ = true;
    return D3D_OK;
}

HRESULT Device::SetStreamSource(UINT stream, IDirect3DVertexBuffer9 *buffer, UINT offset, UINT stride)
{
    if (stream >= kMaxStreams) return D3DERR_INVALIDCALL;
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    Stream &s = streams_[stream];
    Rebind(s.buffer, static_cast<VertexBuffer *>(buffer));
    s.offset = offset;
    if (s.stride != stride) pipelineDirty_ = true;
    s.stride = stride;
    return D3D_OK;
}

HRESULT Device::SetIndices(IDirect3DIndexBuffer9 *buffer)
{
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    Rebind(indices_, static_cast<IndexBuffer *>(buffer));
    return D3D_OK;
}

HRESULT Device::SetVertexDeclaration(IDirect3DVertexDeclaration9 *declaration)
{
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (declaration_ == declaration) return D3D_OK;
    Rebind(declaration_, static_cast<VertexDeclaration *>(declaration));
    pipelineDirty_ = true;
    return D3D_OK;
}

HRESULT Device::SetVertexShader(IDirect3DVertexShader9 *shader)
{
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (vs_ == shader) return D3D_OK;
    Rebind(vs_, static_cast<VertexShader *>(shader));
    pipelineDirty_ = true;
    return D3D_OK;
}

HRESULT Device::SetPixelShader(IDirect3DPixelShader9 *shader)
{
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (ps_ == shader) return D3D_OK;
    Rebind(ps_, static_cast<PixelShader *>(shader));
    pipelineDirty_ = true;
    return D3D_OK;
}

HRESULT Device::SetVertexShaderConstantF(UINT start, const float *data, UINT count)
{
    if (!data || start + count > 256) return D3DERR_INVALIDCALL;
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    std::memcpy(vsConsts_[start], data, count * 16);
    vsConstsDirty_ = true;
    return D3D_OK;
}

HRESULT Device::SetPixelShaderConstantF(UINT start, const float *data, UINT count)
{
    if (!data || start + count > 256) return D3DERR_INVALIDCALL;
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    std::memcpy(psConsts_[start], data, count * 16);
    psConstsDirty_ = true;
    return D3D_OK;
}

// ---- Device: recording -----------------------------------------------------

void Device::EmitTargets()
{
    static bool depthMismatch;
    targetsDirty_ = false;
    TargetsCmd cmd = {};
    TexFormat formats[kMaxTargets] = {};
    unsigned saturate = 0;
    UINT width = 0, height = 0;
    for (UINT i = 0; i < kMaxTargets; ++i) {
        Surface *s = targets_[i];
        if (!s || !s->Gpu().id) continue;
        cmd.colors[i] = s->View();
        formats[i] = s->Gpu().format;
        // Direct3D clamps writes to unorm targets; these two are floats here to stay filterable (BO1's glow chain).
        if (s->Format() == D3DFMT_A16B16G16R16 || s->Format() == D3DFMT_G16R16) saturate |= 1u << i;
        if (!width) width = s->Width(), height = s->Height();
    }
    TexFormat depthFormat = TexFormat::None;
    if (Surface *d = depth_; d && d->Gpu().id) {
        // WebGPU attachments share one size; Direct3D only needs the depth buffer to be as large.
        if (width && (d->Width() != width || d->Height() != height)) {
            WarnOnce(depthMismatch, "depth buffer larger than the render target: drawn without it");
        } else {
            cmd.depth = d->View();
            depthFormat = d->Gpu().format;
            if (!width) width = d->Width(), height = d->Height();
        }
    }
    cmd.width = width;
    cmd.height = height;
    if (std::memcmp(formats, targetFormats_, sizeof formats) || depthFormat != depthFormat_ ||
        saturate != saturateTargets_)
        pipelineDirty_ = true;
    saturateTargets_ = saturate;
    std::memcpy(targetFormats_, formats, sizeof formats);
    depthFormat_ = depthFormat;
    targetWidth_ = width;
    targetHeight_ = height;
    if (!std::memcmp(&cmd, &emittedTargets_, sizeof cmd)) return;
    emittedTargets_ = cmd;
    stream_.Put(Cmd::SetTargets, cmd);
}

template <class S> const ShaderVariant &Device::Variant(S *shader, uint64_t key, const Sm3WgslOptions &options)
{
    auto it = shader->variants.find(key);
    if (it != shader->variants.end()) return it->second;
    Sm3WgslResult r = TranslateSm3ToWgsl(shader->Tokens(), options);
    ShaderVariant v;
    if (r.ok) {
        v.module = nextShader_++;
        v.varyingMask = r.varyingMask;
        stream_.Put(Cmd::CreateShader, ShaderCmd{v.module}, r.wgsl.data(), (uint32_t)r.wgsl.size());
    } else {
        fprintf(stderr, "[webdx] shader translation failed: %s\n", r.error.c_str());
    }
    return shader->variants.emplace(key, v).first->second;
}

bool Device::ResolvePipeline(D3DPRIMITIVETYPE type, uint32_t indexFormat)
{
    pipelineDirty_ = false;
    bindingsDirty_ = true;
    pipeline_.id = 0;
    pipeline_.topology = type;
    pipeline_.indexFormat = indexFormat;

    const VertexDeclaration::Inputs &in = declaration_->GetInputs();
    Sm3WgslOptions vo;
    vo.attribMask = in.attribMask;
    vo.attribUintMask = in.uintMask;
    vo.attribSintMask = in.sintMask;
    const ShaderVariant &vv = Variant(vs_, in.attribMask | (uint64_t)in.uintMask << 16 | (uint64_t)in.sintMask << 32, vo);
    const Sm3WgslResult &pi = ps_->Info(), &vi = vs_->Info();
    unsigned shadow = 0;
    for (UINT s = 0; s < kPixelSamplers; ++s)
        if ((pi.sampler2DMask >> s & 1) && IsDepth(textureGpu_[s].format)) shadow |= 1u << s;
    Sm3WgslOptions po;
    po.shadowMask = shadow;
    po.varyingMask = vv.varyingMask;
    po.saturateMask = saturateTargets_;
    const ShaderVariant &pv =
        Variant(ps_, shadow | (uint64_t)saturateTargets_ << 16 | (uint64_t)vv.varyingMask << 32, po);
    if (!vv.module || !pv.module) return false;

    PipelineCmd p;
    std::memset(&p, 0, sizeof p);
    p.vs = vv.module;
    p.ps = pv.module;
    p.topology = type;
    p.indexFormat = indexFormat;
    uint32_t compact[kMaxStreams] = {};
    for (UINT s = 0; s < kMaxStreams; ++s) {
        if (!(in.streamMask >> s & 1)) continue;
        compact[s] = p.streamCount;
        streamOf_[p.streamCount] = s;
        p.strides[p.streamCount++] = streams_[s].stride;
    }
    for (UINT i = 0; i < in.attrCount; ++i) {
        p.attrs[i] = in.attrs[i];
        p.attrs[i].stream = compact[in.attrs[i].stream];
    }
    p.attrCount = in.attrCount;

    const DWORD *rs = renderStates_;
    static const D3DRENDERSTATETYPE writeMaskStates[kMaxTargets] = {
        D3DRS_COLORWRITEENABLE, D3DRS_COLORWRITEENABLE1, D3DRS_COLORWRITEENABLE2, D3DRS_COLORWRITEENABLE3};
    for (UINT i = 0; i < kMaxTargets; ++i) {
        p.colorFormats[i] = targetFormats_[i];
        // WebGPU wants a shader output for every target it writes.
        if (targetFormats_[i] != TexFormat::None && (pi.colorOutMask >> i & 1))
            p.writeMasks[i] = rs[writeMaskStates[i]] & 0xF;
    }
    if (rs[D3DRS_ALPHABLENDENABLE]) {
        bool separate = rs[D3DRS_SEPARATEALPHABLENDENABLE];
        p.blendEnable = 1;
        p.srcBlend = rs[D3DRS_SRCBLEND];
        p.dstBlend = rs[D3DRS_DESTBLEND];
        p.blendOp = rs[D3DRS_BLENDOP];
        p.srcBlendAlpha = rs[separate ? D3DRS_SRCBLENDALPHA : D3DRS_SRCBLEND];
        p.dstBlendAlpha = rs[separate ? D3DRS_DESTBLENDALPHA : D3DRS_DESTBLEND];
        p.blendOpAlpha = rs[separate ? D3DRS_BLENDOPALPHA : D3DRS_BLENDOP];
    }
    p.depthFormat = depthFormat_;
    if (depthFormat_ != TexFormat::None) {
        p.zEnable = rs[D3DRS_ZENABLE] != D3DZB_FALSE;
        p.zWrite = p.zEnable && rs[D3DRS_ZWRITEENABLE];
        p.zFunc = p.zEnable ? rs[D3DRS_ZFUNC] : D3DCMP_ALWAYS;
        p.depthBias = AsFloat(rs[D3DRS_DEPTHBIAS]);
        p.slopeBias = AsFloat(rs[D3DRS_SLOPESCALEDEPTHBIAS]);
        if (rs[D3DRS_STENCILENABLE]) {
            bool twoSided = rs[D3DRS_TWOSIDEDSTENCILMODE];
            p.stencilEnable = 1;
            p.stencilMask = rs[D3DRS_STENCILMASK] & 0xFF;
            p.stencilWriteMask = rs[D3DRS_STENCILWRITEMASK] & 0xFF;
            p.stencilFunc = rs[D3DRS_STENCILFUNC];
            p.stencilFail = rs[D3DRS_STENCILFAIL];
            p.stencilZFail = rs[D3DRS_STENCILZFAIL];
            p.stencilPass = rs[D3DRS_STENCILPASS];
            p.ccwFunc = rs[twoSided ? D3DRS_CCW_STENCILFUNC : D3DRS_STENCILFUNC];
            p.ccwFail = rs[twoSided ? D3DRS_CCW_STENCILFAIL : D3DRS_STENCILFAIL];
            p.ccwZFail = rs[twoSided ? D3DRS_CCW_STENCILZFAIL : D3DRS_STENCILZFAIL];
            p.ccwPass = rs[twoSided ? D3DRS_CCW_STENCILPASS : D3DRS_STENCILPASS];
        }
    }
    p.cullMode = rs[D3DRS_CULLMODE];
    for (UINT s = 0; s < kSamplers; ++s) {
        const Sm3WgslResult &info = s < kPixelSamplers ? pi : vi;
        UINT r = s < kPixelSamplers ? s : s - kPixelSamplers;
        if (!(info.samplerMask >> r & 1)) continue;
        p.slots[s] = (s < kPixelSamplers && (shadow >> r & 1)) ? kSlotDepth
                     : (info.samplerCubeMask >> r & 1)        ? kSlotCube
                     : (info.samplerVolumeMask >> r & 1)      ? kSlot3D
                                                              : kSlot2D;
    }

    std::string key((const char *)&p, sizeof p);
    auto it = pipelines_.find(key);
    if (it == pipelines_.end()) {
        p.id = nextPipeline_++;
        stream_.Put(Cmd::CreatePipeline, p);
        it = pipelines_.emplace(std::move(key), p.id).first;
    }
    p.id = it->second;
    pipeline_ = p;
    vsMaxConst_ = vi.maxConst;
    psMaxConst_ = pi.maxConst;
    return true;
}

void Device::ResolveBindings()
{
    bindingsDirty_ = false;
    bindings_ = 0;
    BindingsCmd b;
    std::memset(&b, 0, sizeof b);
    bool any = false;
    for (UINT s = 0; s < kSamplers; ++s) {
        uint8_t slot = pipeline_.slots[s];
        if (!slot) continue;
        any = true;
        const GpuTexture &t = textureGpu_[s];
        bool fits = slot == kSlotDepth ? IsDepth(t.format)
                    : slot == kSlotCube ? t.kind == TexKind::Cube
                    : slot == kSlot3D   ? t.kind == TexKind::Tex3D
                                        : t.kind == TexKind::Tex2D && !IsDepth(t.format);
        const DWORD *ss = samplerStates_[s];
        b.slots[s] = slot;
        b.textures[s] = fits ? t.id : 0;
        b.samplers[s] = {ss[D3DSAMP_ADDRESSU],  ss[D3DSAMP_ADDRESSV],  ss[D3DSAMP_ADDRESSW],     ss[D3DSAMP_MAGFILTER],
                         ss[D3DSAMP_MINFILTER], ss[D3DSAMP_MIPFILTER], ss[D3DSAMP_MAXANISOTROPY]};
    }
    if (!any) return;
    std::string key((const char *)&b, sizeof b);
    auto it = bindingSets_.find(key);
    if (it == bindingSets_.end()) {
        b.id = nextBindings_++;
        stream_.Put(Cmd::CreateBindings, b);
        for (UINT s = 0; s < kSamplers; ++s)
            if (b.textures[s]) bindingsByTexture_.emplace(b.textures[s], key);
        it = bindingSets_.emplace(std::move(key), b.id).first;
    }
    bindings_ = it->second;
}

uint32_t Device::Constants(Snapshot &snapshot, bool &dirty, const float (*registers)[4], int maxConst)
{
    uint32_t count = (uint32_t)(maxConst + 1);
    if (!count) return 0;
    if (!snapshot.valid || dirty || snapshot.count < count) {
        snapshot = {true, stream_.Uniform(registers, count * 16), count};
        dirty = false;
    }
    return snapshot.offset;
}

uint32_t Device::Params(float viewportWidth, float viewportHeight)
{
    DrawParams p = {};
    if (renderStates_[D3DRS_ALPHATESTENABLE]) {
        p.alphaFunc = (int32_t)renderStates_[D3DRS_ALPHAFUNC];
        p.alphaRef = (renderStates_[D3DRS_ALPHAREF] & 0xFF) / 255.0f;
    }
    if (viewportWidth > 0 && viewportHeight > 0) {
        p.pixelShift[0] = 1.0f / viewportWidth;
        p.pixelShift[1] = -1.0f / viewportHeight;
    }
    if (!paramsValid_ || std::memcmp(&p, &params_, sizeof p)) {
        params_ = p;
        paramsOffset_ = stream_.Uniform(&p, sizeof p);
        paramsValid_ = true;
    }
    return paramsOffset_;
}

HRESULT Device::Draw(D3DPRIMITIVETYPE type, UINT primCount, bool indexed, UINT first, INT baseVertex)
{
    static bool unsupported;
    UINT count = type == D3DPT_TRIANGLELIST    ? primCount * 3
                 : type == D3DPT_TRIANGLESTRIP ? primCount + 2
                 : type == D3DPT_LINELIST      ? primCount * 2
                 : type == D3DPT_LINESTRIP     ? primCount + 1
                 : type == D3DPT_POINTLIST     ? primCount
                                               : 0;
    if (!count) {
        WarnOnce(unsupported, "triangle fans are not drawn");
        return D3D_OK;
    }
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!vs_ || !ps_ || !declaration_ || (indexed && !indices_)) return D3D_OK;
    if (targetsDirty_) EmitTargets();
    if (!targetWidth_) return D3D_OK;
    bool strip = type == D3DPT_TRIANGLESTRIP || type == D3DPT_LINESTRIP;
    uint32_t indexFormat = indexed && strip ? (indices_->Format() == D3DFMT_INDEX16 ? 16 : 32) : 0;
    if ((DWORD)type != pipeline_.topology || indexFormat != pipeline_.indexFormat) pipelineDirty_ = true;
    if (pipelineDirty_) ResolvePipeline(type, indexFormat);
    if (!pipeline_.id) return D3D_OK;
    if (bindingsDirty_) ResolveBindings();

    DrawCmd cmd;
    std::memset(&cmd, 0, sizeof cmd);
    cmd.pipeline = pipeline_.id;
    cmd.bindings = bindings_;
    float x = (float)std::min<DWORD>(viewport_.X, targetWidth_), y = (float)std::min<DWORD>(viewport_.Y, targetHeight_);
    cmd.viewport[0] = x;
    cmd.viewport[1] = y;
    cmd.viewport[2] = std::min((float)viewport_.Width, targetWidth_ - x);
    cmd.viewport[3] = std::min((float)viewport_.Height, targetHeight_ - y);
    cmd.viewport[4] = viewport_.MinZ;
    cmd.viewport[5] = viewport_.MaxZ;
    RECT sc = {0, 0, (LONG)targetWidth_, (LONG)targetHeight_};
    if (renderStates_[D3DRS_SCISSORTESTENABLE]) {
        sc.left = std::clamp<LONG>(scissor_.left, 0, sc.right);
        sc.top = std::clamp<LONG>(scissor_.top, 0, sc.bottom);
        sc.right = std::clamp<LONG>(scissor_.right, sc.left, sc.right);
        sc.bottom = std::clamp<LONG>(scissor_.bottom, sc.top, sc.bottom);
    }
    cmd.scissor[0] = sc.left;
    cmd.scissor[1] = sc.top;
    cmd.scissor[2] = sc.right - sc.left;
    cmd.scissor[3] = sc.bottom - sc.top;
    cmd.stencilRef = renderStates_[D3DRS_STENCILREF] & 0xFF;
    cmd.blendFactor = renderStates_[kBlendFactor];
    for (UINT i = 0; i < pipeline_.streamCount; ++i) {
        const Stream &s = streams_[streamOf_[i]];
        if (!s.buffer) return D3D_OK;
        cmd.vertexBuffers[i] = s.buffer->GpuId();
        cmd.vertexOffsets[i] = s.offset;
    }
    if (indexed) {
        cmd.indexBuffer = indices_->GpuId();
        cmd.indexFormat = indices_->Format() == D3DFMT_INDEX16 ? 16 : 32;
    }
    cmd.uniforms[0] = Constants(vsSnapshot_, vsConstsDirty_, vsConsts_, vsMaxConst_);
    cmd.uniforms[1] = Constants(psSnapshot_, psConstsDirty_, psConsts_, psMaxConst_);
    cmd.uniforms[2] = Params(cmd.viewport[2], cmd.viewport[3]);
    cmd.count = count;
    cmd.first = first;
    cmd.baseVertex = baseVertex;
    stream_.Put(Cmd::Draw, cmd);
    return D3D_OK;
}

HRESULT Device::DrawPrimitive(D3DPRIMITIVETYPE type, UINT startVertex, UINT primCount)
{
    return Draw(type, primCount, false, startVertex, 0);
}

HRESULT Device::DrawIndexedPrimitive(D3DPRIMITIVETYPE type, INT baseVertex, UINT, UINT, UINT startIndex,
                                     UINT primCount)
{
    return Draw(type, primCount, true, startIndex, baseVertex);
}

HRESULT WINAPI Device::DrawPrimitiveUP(D3DPRIMITIVETYPE, UINT, const void *, UINT)
{
    static bool warned;
    WarnOnce(warned, "DrawPrimitiveUP is not drawn");
    return D3D_OK;
}

HRESULT WINAPI Device::Clear(DWORD count, const D3DRECT *rects, DWORD flags, D3DCOLOR color, float z, DWORD stencil)
{
    static bool partial;
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (targetsDirty_) EmitTargets();
    if (!targetWidth_) return D3D_OK;
    // Direct3D clears the viewport, narrowed by the scissor test and the rectangles.
    RECT area = {(LONG)viewport_.X, (LONG)viewport_.Y, (LONG)(viewport_.X + viewport_.Width),
                 (LONG)(viewport_.Y + viewport_.Height)};
    if (renderStates_[D3DRS_SCISSORTESTENABLE]) {
        area.left = std::max(area.left, scissor_.left);
        area.top = std::max(area.top, scissor_.top);
        area.right = std::min(area.right, scissor_.right);
        area.bottom = std::min(area.bottom, scissor_.bottom);
    }
    if (count == 1 && rects) {
        area.left = std::max(area.left, rects[0].x1);
        area.top = std::max(area.top, rects[0].y1);
        area.right = std::min(area.right, rects[0].x2);
        area.bottom = std::min(area.bottom, rects[0].y2);
    }
    if (count > 1 || area.left > 0 || area.top > 0 || area.right < (LONG)targetWidth_ ||
        area.bottom < (LONG)targetHeight_) {
        WarnOnce(partial, "partial clears are not done");
        return D3D_OK;
    }
    ClearCmd cmd = {flags,
                    {((color >> 16) & 0xFF) / 255.0f, ((color >> 8) & 0xFF) / 255.0f, (color & 0xFF) / 255.0f,
                     ((color >> 24) & 0xFF) / 255.0f},
                    z,
                    stencil & 0xFF};
    stream_.Put(Cmd::Clear, cmd);
    return D3D_OK;
}

HRESULT WINAPI Device::StretchRect(IDirect3DSurface9 *src, const RECT *srcRect, IDirect3DSurface9 *dst,
                                   const RECT *dstRect, D3DTEXTUREFILTERTYPE filter)
{
    static bool unsupported;
    if (!src || !dst) return D3DERR_INVALIDCALL;
    Surface *s = static_cast<Surface *>(src), *d = static_cast<Surface *>(dst);
    if (!s->Gpu().id || !d->Gpu().id || IsDepth(s->Gpu().format) || IsDepth(d->Gpu().format)) {
        WarnOnce(unsupported, "StretchRect between depth or null surfaces is not done");
        return D3D_OK;
    }
    RECT sr = srcRect ? *srcRect : RECT{0, 0, (LONG)s->Width(), (LONG)s->Height()};
    RECT dr = dstRect ? *dstRect : RECT{0, 0, (LONG)d->Width(), (LONG)d->Height()};
    BlitParams params = {(float)sr.left / s->Width(), (float)sr.top / s->Height(), (float)sr.right / s->Width(),
                         (float)sr.bottom / s->Height()};
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    CopyCmd cmd = {s->View(), d->View(), {dr.left, dr.top, dr.right - dr.left, dr.bottom - dr.top},
                   filter == D3DTEXF_LINEAR ? 1u : 0u, stream_.Uniform(&params, sizeof params)};
    stream_.Put(Cmd::Copy, cmd);
    return D3D_OK;
}

HRESULT WINAPI Device::Present(const RECT *, const RECT *, HWND, const RGNDATA *)
{
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    BlitParams params = {0, 0, 1, 1};
    stream_.Put(Cmd::Present, PresentCmd{backBuffer_->Gpu().id, stream_.Uniform(&params, sizeof params)});
    Flush();
    return D3D_OK;
}

void Device::Flush()
{
    executor_->Submit(stream_.TakeCommands(), stream_.TakeUniforms());
    vsSnapshot_.valid = psSnapshot_.valid = paramsValid_ = false;
    ++submitted_;
    while (submitted_ - executor_->Completed() > 2)
        usleep(500);
}

} // namespace webdx
