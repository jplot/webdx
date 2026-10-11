// webgpu.cpp — the WebGPU executor: owns the device and #canvas on the page's main thread, which
// presents each frame when it returns to its event loop.
#include "executor.h"
#include "stream.h"

#include <d3d9.h>
#include <webgpu/webgpu.h>

#include <emscripten.h>
#include <emscripten/html5.h>
#include <emscripten/proxying.h>
#include <emscripten/threading.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <unistd.h>
#include <unordered_map>
#include <vector>

namespace webdx {
namespace {

WGPUStringView View(const char *s) { return WGPUStringView{s, strlen(s)}; }

WGPUTextureFormat TextureFormat(TexFormat f)
{
    switch (f) {
    case TexFormat::BGRA8: return WGPUTextureFormat_BGRA8Unorm;
    case TexFormat::RGBA8: return WGPUTextureFormat_RGBA8Unorm;
    case TexFormat::RGBA16F: return WGPUTextureFormat_RGBA16Float;
    case TexFormat::RG32F: return WGPUTextureFormat_RG32Float;
    case TexFormat::R32F: return WGPUTextureFormat_R32Float;
    case TexFormat::BC1: return WGPUTextureFormat_BC1RGBAUnorm;
    case TexFormat::BC2: return WGPUTextureFormat_BC2RGBAUnorm;
    case TexFormat::BC3: return WGPUTextureFormat_BC3RGBAUnorm;
    case TexFormat::Depth24S8: return WGPUTextureFormat_Depth24PlusStencil8;
    case TexFormat::Depth32FS8: return WGPUTextureFormat_Depth32FloatStencil8;
    default: return WGPUTextureFormat_Undefined;
    }
}

WGPUBlendFactor BlendFactor(uint32_t blend)   // D3DBLEND
{
    switch (blend) {
    case 1: return WGPUBlendFactor_Zero;
    case 3: return WGPUBlendFactor_Src;
    case 4: return WGPUBlendFactor_OneMinusSrc;
    case 5: return WGPUBlendFactor_SrcAlpha;
    case 6: return WGPUBlendFactor_OneMinusSrcAlpha;
    case 7: return WGPUBlendFactor_DstAlpha;
    case 8: return WGPUBlendFactor_OneMinusDstAlpha;
    case 9: return WGPUBlendFactor_Dst;
    case 10: return WGPUBlendFactor_OneMinusDst;
    case 11: return WGPUBlendFactor_SrcAlphaSaturated;
    case 14: return WGPUBlendFactor_Constant;
    case 15: return WGPUBlendFactor_OneMinusConstant;
    default: return WGPUBlendFactor_One;
    }
}

WGPUBlendComponent Blend(uint32_t op, uint32_t src, uint32_t dst)   // D3DBLENDOP
{
    static const WGPUBlendOperation ops[] = {WGPUBlendOperation_Add, WGPUBlendOperation_Add,
                                             WGPUBlendOperation_Subtract, WGPUBlendOperation_ReverseSubtract,
                                             WGPUBlendOperation_Min, WGPUBlendOperation_Max};
    WGPUBlendOperation operation = op < 6 ? ops[op] : WGPUBlendOperation_Add;
    // Direct3D ignores the factors of MIN and MAX; WebGPU requires them to be one.
    bool minMax = operation == WGPUBlendOperation_Min || operation == WGPUBlendOperation_Max;
    return {operation, minMax ? WGPUBlendFactor_One : BlendFactor(src), minMax ? WGPUBlendFactor_One : BlendFactor(dst)};
}

WGPUCompareFunction Compare(uint32_t func)   // D3DCMPFUNC
{
    static const WGPUCompareFunction funcs[] = {
        WGPUCompareFunction_Always,  WGPUCompareFunction_Never,        WGPUCompareFunction_Less,
        WGPUCompareFunction_Equal,   WGPUCompareFunction_LessEqual,    WGPUCompareFunction_Greater,
        WGPUCompareFunction_NotEqual, WGPUCompareFunction_GreaterEqual, WGPUCompareFunction_Always};
    return func < 9 ? funcs[func] : WGPUCompareFunction_Always;
}

WGPUStencilOperation StencilOp(uint32_t op)   // D3DSTENCILOP
{
    static const WGPUStencilOperation ops[] = {
        WGPUStencilOperation_Keep,           WGPUStencilOperation_Keep,           WGPUStencilOperation_Zero,
        WGPUStencilOperation_Replace,        WGPUStencilOperation_IncrementClamp, WGPUStencilOperation_DecrementClamp,
        WGPUStencilOperation_Invert,         WGPUStencilOperation_IncrementWrap,  WGPUStencilOperation_DecrementWrap};
    return op < 9 ? ops[op] : WGPUStencilOperation_Keep;
}

WGPUVertexFormat VertexFormat(uint32_t type)   // D3DDECLTYPE
{
    static const WGPUVertexFormat formats[] = {
        WGPUVertexFormat_Float32,   WGPUVertexFormat_Float32x2,   WGPUVertexFormat_Float32x3,
        WGPUVertexFormat_Float32x4, WGPUVertexFormat_Unorm8x4BGRA, WGPUVertexFormat_Uint8x4,
        WGPUVertexFormat_Sint16x2,  WGPUVertexFormat_Sint16x4,    WGPUVertexFormat_Unorm8x4,
        WGPUVertexFormat_Snorm16x2, WGPUVertexFormat_Snorm16x4,   WGPUVertexFormat_Unorm16x2,
        WGPUVertexFormat_Unorm16x4, WGPUVertexFormat_Force32,     WGPUVertexFormat_Force32,
        WGPUVertexFormat_Float16x2, WGPUVertexFormat_Float16x4};
    return type < 17 ? formats[type] : WGPUVertexFormat_Force32;
}

WGPUPrimitiveTopology Topology(uint32_t type)   // D3DPRIMITIVETYPE
{
    switch (type) {
    case D3DPT_POINTLIST: return WGPUPrimitiveTopology_PointList;
    case D3DPT_LINELIST: return WGPUPrimitiveTopology_LineList;
    case D3DPT_LINESTRIP: return WGPUPrimitiveTopology_LineStrip;
    case D3DPT_TRIANGLESTRIP: return WGPUPrimitiveTopology_TriangleStrip;
    default: return WGPUPrimitiveTopology_TriangleList;
    }
}

WGPUAddressMode Address(uint32_t mode)   // D3DTEXTUREADDRESS
{
    switch (mode) {
    case 1: return WGPUAddressMode_Repeat;
    case 2: case 5: return WGPUAddressMode_MirrorRepeat;
    default: return WGPUAddressMode_ClampToEdge;
    }
}

WGPUIndexFormat IndexFormat(uint32_t bits) { return bits == 16 ? WGPUIndexFormat_Uint16 : WGPUIndexFormat_Uint32; }

const char kBlitWgsl[] = R"(
struct Params { rect: vec4f }
@group(0) @binding(0) var<uniform> params: Params;
@group(1) @binding(0) var source: texture_2d<f32>;
@group(1) @binding(1) var sourceSampler: sampler;
struct VertexOut { @builtin(position) pos: vec4f, @location(0) uv: vec2f }
@vertex fn vs(@builtin(vertex_index) i: u32) -> VertexOut {
    let corner = vec2f(f32((i << 1u) & 2u), f32(i & 2u));
    var out: VertexOut;
    out.pos = vec4f(corner.x * 2.0 - 1.0, 1.0 - corner.y * 2.0, 0.0, 1.0);
    out.uv = mix(params.rect.xy, params.rect.zw, corner);
    return out;
}
@fragment fn fs(in: VertexOut) -> @location(0) vec4f { return textureSample(source, sourceSampler, in.uv); }
)";

template <class T> T &Slot(std::vector<T> &v, uint32_t id)
{
    if (id >= v.size()) v.resize(id + 1);
    return v[id];
}

class WebGpuExecutor final : public Executor {
public:
    WebGpuExecutor(int width, int height) : width_(width), height_(height) {}

    bool Start()
    {
        emscripten_proxy_async(emscripten_proxy_get_system_queue(), emscripten_main_runtime_thread_id(),
                               [](void *self) { static_cast<WebGpuExecutor *>(self)->RequestAdapter(); }, this);
        while (state_.load() == 0)
            usleep(1000);
        return state_.load() > 0;
    }

    void Submit(std::vector<uint8_t> &&commands, std::vector<uint8_t> &&uniforms) override
    {
        auto *job = new Job{this, std::move(commands), std::move(uniforms)};
        emscripten_proxy_async(emscripten_proxy_get_system_queue(), emscripten_main_runtime_thread_id(),
                               [](void *p) {
                                   Job *job = static_cast<Job *>(p);
                                   job->executor->Run(job->commands, job->uniforms);
                                   job->executor->completed_.fetch_add(1);
                                   delete job;
                               },
                               job);
    }

    uint32_t Completed() const override { return completed_.load(); }

private:
    struct Job {
        WebGpuExecutor *executor;
        std::vector<uint8_t> commands, uniforms;
    };
    struct Buffer {
        WGPUBuffer handle = nullptr;
        uint32_t segment = 0;
    };
    struct Texture {
        WGPUTexture handle = nullptr;
        TextureCmd desc = {};
        WGPUTextureView sampled = nullptr;
        std::unordered_map<uint32_t, WGPUTextureView> views;   // one level of one layer, by level << 16 | layer
        std::vector<uint32_t> bindings;                         // binding sets that sample it
    };
    struct Pipeline {
        WGPURenderPipeline handle = nullptr;
        uint32_t streamCount = 0;
        bool sampled = false;
    };
    struct Bindings {
        WGPUBindGroup group = nullptr;
        uint32_t segment = 0;
    };
    struct Layout {
        WGPUBindGroupLayout group1 = nullptr;
        WGPUPipelineLayout pipeline = nullptr;
    };

    void RequestAdapter()
    {
        instance_ = wgpuCreateInstance(nullptr);
        WGPURequestAdapterCallbackInfo info = WGPU_REQUEST_ADAPTER_CALLBACK_INFO_INIT;
        info.mode = WGPUCallbackMode_AllowSpontaneous;
        info.userdata1 = this;
        info.callback = [](WGPURequestAdapterStatus status, WGPUAdapter adapter, WGPUStringView message, void *self, void *) {
            static_cast<WebGpuExecutor *>(self)->OnAdapter(status, adapter, message);
        };
        wgpuInstanceRequestAdapter(instance_, nullptr, info);
    }

    void OnAdapter(WGPURequestAdapterStatus status, WGPUAdapter adapter, WGPUStringView message)
    {
        if (status != WGPURequestAdapterStatus_Success) {
            fprintf(stderr, "[webdx] WebGPU adapter unavailable: %.*s\n", (int)message.length, message.data);
            state_ = -1;
            return;
        }
        adapter_ = adapter;
        static const WGPUFeatureName features[] = {WGPUFeatureName_TextureCompressionBC,
                                                   WGPUFeatureName_Float32Filterable,
                                                   WGPUFeatureName_Depth32FloatStencil8};
        for (WGPUFeatureName f : features) {
            if (!wgpuAdapterHasFeature(adapter, f)) {
                fprintf(stderr, "[webdx] WebGPU adapter lacks feature %d\n", (int)f);
                state_ = -1;
                return;
            }
        }
        WGPUDeviceDescriptor desc = WGPU_DEVICE_DESCRIPTOR_INIT;
        desc.requiredFeatureCount = sizeof features / sizeof features[0];
        desc.requiredFeatures = features;
        desc.uncapturedErrorCallbackInfo.callback = [](WGPUDevice const *, WGPUErrorType, WGPUStringView message,
                                                       void *, void *) {
            static int reported;
            if (reported++ < 20) fprintf(stderr, "[webdx] WebGPU error: %.*s\n", (int)message.length, message.data);
        };
        WGPURequestDeviceCallbackInfo info = WGPU_REQUEST_DEVICE_CALLBACK_INFO_INIT;
        info.mode = WGPUCallbackMode_AllowSpontaneous;
        info.userdata1 = this;
        info.callback = [](WGPURequestDeviceStatus status, WGPUDevice device, WGPUStringView message, void *self, void *) {
            static_cast<WebGpuExecutor *>(self)->OnDevice(status, device, message);
        };
        wgpuAdapterRequestDevice(adapter, &desc, info);
    }

    void OnDevice(WGPURequestDeviceStatus status, WGPUDevice device, WGPUStringView message)
    {
        if (status != WGPURequestDeviceStatus_Success) {
            fprintf(stderr, "[webdx] WebGPU device unavailable: %.*s\n", (int)message.length, message.data);
            state_ = -1;
            return;
        }
        device_ = device;
        queue_ = wgpuDeviceGetQueue(device);
        emscripten_set_canvas_element_size("#canvas", width_, height_);
        WGPUEmscriptenSurfaceSourceCanvasHTMLSelector canvas = WGPU_EMSCRIPTEN_SURFACE_SOURCE_CANVAS_HTML_SELECTOR_INIT;
        canvas.selector = View("#canvas");
        WGPUSurfaceDescriptor surfaceDesc = WGPU_SURFACE_DESCRIPTOR_INIT;
        surfaceDesc.nextInChain = &canvas.chain;
        surface_ = wgpuInstanceCreateSurface(instance_, &surfaceDesc);
        WGPUSurfaceCapabilities caps = WGPU_SURFACE_CAPABILITIES_INIT;
        wgpuSurfaceGetCapabilities(surface_, adapter_, &caps);
        surfaceFormat_ = caps.formats[0];
        WGPUSurfaceConfiguration config = WGPU_SURFACE_CONFIGURATION_INIT;
        config.device = device;
        config.format = surfaceFormat_;
        config.width = (uint32_t)width_;
        config.height = (uint32_t)height_;
        wgpuSurfaceConfigure(surface_, &config);
        wgpuSurfaceCapabilitiesFreeMembers(caps);
        CreateFixedObjects();
        fprintf(stderr, "[webdx] WebGPU device ready (%dx%d)\n", width_, height_);
        state_ = 1;
    }

    void CreateFixedObjects()
    {
        WGPUBindGroupLayoutEntry e[5];
        for (int i = 0; i < 5; ++i) {
            e[i] = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
            e[i].binding = i;
            e[i].visibility = i == 0 || i == 3 ? WGPUShaderStage_Vertex : WGPUShaderStage_Fragment;
            e[i].buffer.type = WGPUBufferBindingType_Uniform;
            e[i].buffer.hasDynamicOffset = i < 3;
            e[i].buffer.minBindingSize = i < 2 ? kConstantBlock : i == 2 ? sizeof(PixelParams) : 256;
        }
        WGPUBindGroupLayoutDescriptor bgl = WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT;
        bgl.entryCount = 5;
        bgl.entries = e;
        layout0_ = wgpuDeviceCreateBindGroupLayout(device_, &bgl);

        WGPUBufferDescriptor zeros = WGPU_BUFFER_DESCRIPTOR_INIT;
        zeros.usage = WGPUBufferUsage_Uniform;
        zeros.size = 256;
        zeros_ = wgpuDeviceCreateBuffer(device_, &zeros);

        static const uint8_t black[4] = {0, 0, 0, 255};
        placeholders_[kSlot2D] = Placeholder(WGPUTextureDimension_2D, WGPUTextureViewDimension_2D, 1, black);
        placeholders_[kSlotCube] = Placeholder(WGPUTextureDimension_2D, WGPUTextureViewDimension_Cube, 6, black);
        placeholders_[kSlot3D] = Placeholder(WGPUTextureDimension_3D, WGPUTextureViewDimension_3D, 1, black);
        placeholders_[kSlotDepth] = Placeholder(WGPUTextureDimension_2D, WGPUTextureViewDimension_2D, 1, nullptr);

        WGPUBindGroupLayoutEntry b0 = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
        b0.binding = 0;
        b0.visibility = WGPUShaderStage_Vertex;
        b0.buffer.type = WGPUBufferBindingType_Uniform;
        b0.buffer.hasDynamicOffset = true;
        b0.buffer.minBindingSize = sizeof(BlitParams);
        bgl.entryCount = 1;
        bgl.entries = &b0;
        blitLayout0_ = wgpuDeviceCreateBindGroupLayout(device_, &bgl);
        WGPUBindGroupLayoutEntry b1[2] = {WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT, WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT};
        b1[0].binding = 0;
        b1[0].visibility = WGPUShaderStage_Fragment;
        b1[0].texture.sampleType = WGPUTextureSampleType_Float;
        b1[0].texture.viewDimension = WGPUTextureViewDimension_2D;
        b1[1].binding = 1;
        b1[1].visibility = WGPUShaderStage_Fragment;
        b1[1].sampler.type = WGPUSamplerBindingType_Filtering;
        bgl.entryCount = 2;
        bgl.entries = b1;
        blitLayout1_ = wgpuDeviceCreateBindGroupLayout(device_, &bgl);
        WGPUBindGroupLayout groups[2] = {blitLayout0_, blitLayout1_};
        WGPUPipelineLayoutDescriptor pl = WGPU_PIPELINE_LAYOUT_DESCRIPTOR_INIT;
        pl.bindGroupLayoutCount = 2;
        pl.bindGroupLayouts = groups;
        blitLayout_ = wgpuDeviceCreatePipelineLayout(device_, &pl);
        blitShader_ = ShaderModule(kBlitWgsl, sizeof kBlitWgsl - 1);
    }

    WGPUTextureView Placeholder(WGPUTextureDimension dimension, WGPUTextureViewDimension view, uint32_t layers,
                                const uint8_t *texel)
    {
        WGPUTextureDescriptor d = WGPU_TEXTURE_DESCRIPTOR_INIT;
        d.dimension = dimension;
        d.size = {1, 1, layers};
        d.format = texel ? WGPUTextureFormat_RGBA8Unorm : WGPUTextureFormat_Depth24PlusStencil8;
        d.usage = WGPUTextureUsage_TextureBinding | (texel ? WGPUTextureUsage_CopyDst : 0);
        WGPUTexture texture = wgpuDeviceCreateTexture(device_, &d);
        for (uint32_t layer = 0; texel && layer < layers; ++layer) {
            WGPUTexelCopyTextureInfo dst = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
            dst.texture = texture;
            dst.origin = {0, 0, layer};
            WGPUTexelCopyBufferLayout layout = WGPU_TEXEL_COPY_BUFFER_LAYOUT_INIT;
            layout.bytesPerRow = 4;
            WGPUExtent3D size = {1, 1, 1};
            wgpuQueueWriteTexture(queue_, &dst, texel, 4, &layout, &size);
        }
        WGPUTextureViewDescriptor v = WGPU_TEXTURE_VIEW_DESCRIPTOR_INIT;
        v.dimension = view;
        v.aspect = texel ? WGPUTextureAspect_All : WGPUTextureAspect_DepthOnly;
        return wgpuTextureCreateView(texture, &v);
    }

    WGPUShaderModule ShaderModule(const char *text, size_t size)
    {
        WGPUShaderSourceWGSL source = WGPU_SHADER_SOURCE_WGSL_INIT;
        source.code = WGPUStringView{text, size};
        WGPUShaderModuleDescriptor d = WGPU_SHADER_MODULE_DESCRIPTOR_INIT;
        d.nextInChain = &source.chain;
        return wgpuDeviceCreateShaderModule(device_, &d);
    }

    void Run(const std::vector<uint8_t> &commands, const std::vector<uint8_t> &uniforms)
    {
        if (!uniforms.empty()) UploadUniforms(uniforms);
        StreamReader reader(commands.data(), commands.size());
        Cmd cmd;
        const uint8_t *p;
        uint32_t size;
        while (reader.Next(cmd, p, size)) {
            switch (cmd) {
            case Cmd::CreateBuffer: CreateBuffer(StreamReader::Read<BufferCmd>(p)); break;
            case Cmd::WriteBuffer:
                WriteBuffer(StreamReader::Read<BufferWriteCmd>(p), p + sizeof(BufferWriteCmd),
                            size - sizeof(BufferWriteCmd));
                break;
            case Cmd::RenameBuffer: RenameBuffer(StreamReader::Read<BufferCmd>(p)); break;
            case Cmd::DestroyBuffer: DestroyBuffer(StreamReader::Read<BufferCmd>(p)); break;
            case Cmd::CreateTexture: CreateTexture(StreamReader::Read<TextureCmd>(p)); break;
            case Cmd::WriteTexture:
                WriteTexture(StreamReader::Read<TextureWriteCmd>(p), p + sizeof(TextureWriteCmd),
                             size - sizeof(TextureWriteCmd));
                break;
            case Cmd::DestroyTexture: DestroyTexture(StreamReader::Read<TextureCmd>(p)); break;
            case Cmd::CreateShader:
                Slot(shaders_, StreamReader::Read<ShaderCmd>(p).id) =
                    ShaderModule((const char *)p + sizeof(ShaderCmd), size - sizeof(ShaderCmd));
                break;
            case Cmd::CreatePipeline: CreatePipeline(StreamReader::Read<PipelineCmd>(p)); break;
            case Cmd::CreateBindings: CreateBindings(StreamReader::Read<BindingsCmd>(p)); break;
            case Cmd::DestroyBindings: DestroyBindings(StreamReader::Read<BindingsCmd>(p)); break;
            case Cmd::SetTargets: SetTargets(StreamReader::Read<TargetsCmd>(p)); break;
            case Cmd::Clear: Clear(StreamReader::Read<ClearCmd>(p)); break;
            case Cmd::Draw: Draw(StreamReader::Read<DrawCmd>(p)); break;
            case Cmd::Copy: Copy(StreamReader::Read<CopyCmd>(p)); break;
            case Cmd::Present: Present(StreamReader::Read<PresentCmd>(p)); break;
            }
        }
    }

    void UploadUniforms(const std::vector<uint8_t> &uniforms)
    {
        // Draws still to submit read the previous uniforms.
        if (segmentUsesUniforms_) FlushSegment();
        if (uniforms.size() > uniformCapacity_) {
            uniformCapacity_ = 1u << 20;
            while (uniformCapacity_ < uniforms.size()) uniformCapacity_ <<= 1;
            if (uniforms_) wgpuBufferRelease(uniforms_);
            WGPUBufferDescriptor d = WGPU_BUFFER_DESCRIPTOR_INIT;
            d.usage = WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst;
            d.size = uniformCapacity_;
            uniforms_ = wgpuDeviceCreateBuffer(device_, &d);

            WGPUBindGroupEntry e[5];
            for (int i = 0; i < 5; ++i) {
                e[i] = WGPU_BIND_GROUP_ENTRY_INIT;
                e[i].binding = i;
                e[i].buffer = i < 3 ? uniforms_ : zeros_;
                e[i].size = i < 2 ? kConstantBlock : i == 2 ? sizeof(PixelParams) : 256;
            }
            WGPUBindGroupDescriptor g = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
            g.layout = layout0_;
            g.entryCount = 5;
            g.entries = e;
            if (uniformGroup_) wgpuBindGroupRelease(uniformGroup_);
            uniformGroup_ = wgpuDeviceCreateBindGroup(device_, &g);
            e[0].size = sizeof(BlitParams);
            g.layout = blitLayout0_;
            g.entryCount = 1;
            if (blitGroup_) wgpuBindGroupRelease(blitGroup_);
            blitGroup_ = wgpuDeviceCreateBindGroup(device_, &g);
        }
        wgpuQueueWriteBuffer(queue_, uniforms_, 0, uniforms.data(), uniforms.size());
    }

    WGPUCommandEncoder Encoder()
    {
        if (!encoder_) encoder_ = wgpuDeviceCreateCommandEncoder(device_, nullptr);
        return encoder_;
    }

    void FlushSegment()
    {
        EndPass();
        if (encoder_) {
            WGPUCommandBuffer commands = wgpuCommandEncoderFinish(encoder_, nullptr);
            wgpuQueueSubmit(queue_, 1, &commands);
            wgpuCommandBufferRelease(commands);
            wgpuCommandEncoderRelease(encoder_);
            encoder_ = nullptr;
        }
        // Destroyed only now: draws of the segment may have used them.
        for (WGPUTexture t : deadTextures_) {
            wgpuTextureDestroy(t);
            wgpuTextureRelease(t);
        }
        for (WGPUBuffer b : deadBuffers_) {
            wgpuBufferDestroy(b);
            wgpuBufferRelease(b);
        }
        deadTextures_.clear();
        deadBuffers_.clear();
        ++segment_;
        segmentUsesUniforms_ = false;
    }

    WGPUBuffer NewBuffer(uint32_t size)
    {
        WGPUBufferDescriptor d = WGPU_BUFFER_DESCRIPTOR_INIT;
        d.usage = WGPUBufferUsage_Vertex | WGPUBufferUsage_Index | WGPUBufferUsage_CopyDst;
        d.size = size ? size : 4;
        return wgpuDeviceCreateBuffer(device_, &d);
    }

    void CreateBuffer(const BufferCmd &c)
    {
        Slot(buffers_, c.id).handle = NewBuffer(c.size);
    }

    void RenameBuffer(const BufferCmd &c)
    {
        Buffer &b = buffers_[c.id];
        deadBuffers_.push_back(b.handle);
        b.handle = NewBuffer(c.size);
        b.segment = 0;
    }

    void WriteBuffer(const BufferWriteCmd &c, const uint8_t *data, uint32_t size)
    {
        Buffer &b = buffers_[c.id];
        // Without D3DLOCK_NOOVERWRITE, draws already recorded must still see the old contents.
        if (!c.noOverwrite && b.segment == segment_) FlushSegment();
        wgpuQueueWriteBuffer(queue_, b.handle, c.offset, data, size);
    }

    void DestroyBuffer(const BufferCmd &c)
    {
        Buffer &b = buffers_[c.id];
        deadBuffers_.push_back(b.handle);
        b = Buffer{};
    }

    void CreateTexture(const TextureCmd &c)
    {
        Texture &t = Slot(textures_, c.id);
        bool depth = IsDepth(c.format);
        WGPUTextureDescriptor d = WGPU_TEXTURE_DESCRIPTOR_INIT;
        d.dimension = c.kind == TexKind::Tex3D ? WGPUTextureDimension_3D : WGPUTextureDimension_2D;
        d.size = {c.width, c.height, c.kind == TexKind::Cube ? 6u : c.depth};
        d.format = TextureFormat(c.format);
        d.mipLevelCount = c.levels;
        d.usage = WGPUTextureUsage_TextureBinding | (depth ? 0 : WGPUTextureUsage_CopyDst) |
                  (c.renderable ? WGPUTextureUsage_RenderAttachment : 0);
        t.handle = wgpuDeviceCreateTexture(device_, &d);
        t.desc = c;
        WGPUTextureViewDescriptor v = WGPU_TEXTURE_VIEW_DESCRIPTOR_INIT;
        v.dimension = c.kind == TexKind::Cube    ? WGPUTextureViewDimension_Cube
                      : c.kind == TexKind::Tex3D ? WGPUTextureViewDimension_3D
                                                 : WGPUTextureViewDimension_2D;
        v.aspect = depth ? WGPUTextureAspect_DepthOnly : WGPUTextureAspect_All;
        t.sampled = wgpuTextureCreateView(t.handle, &v);
    }

    void WriteTexture(const TextureWriteCmd &c, const uint8_t *data, uint32_t size)
    {
        Texture &t = textures_[c.id];
        for (uint32_t b : t.bindings) {
            if (b < bindings_.size() && bindings_[b].group && bindings_[b].segment == segment_) {
                FlushSegment();
                break;
            }
        }
        WGPUTexelCopyTextureInfo dst = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
        dst.texture = t.handle;
        dst.mipLevel = c.level;
        dst.origin = {c.x, c.y, c.z};
        WGPUTexelCopyBufferLayout layout = WGPU_TEXEL_COPY_BUFFER_LAYOUT_INIT;
        layout.bytesPerRow = c.bytesPerRow;
        layout.rowsPerImage = c.rowsPerImage;
        WGPUExtent3D extent = {c.width, c.height, c.depth};
        wgpuQueueWriteTexture(queue_, &dst, data, size, &layout, &extent);
    }

    void DestroyTexture(const TextureCmd &c)
    {
        Texture &t = textures_[c.id];
        wgpuTextureViewRelease(t.sampled);
        for (auto &v : t.views) wgpuTextureViewRelease(v.second);
        deadTextures_.push_back(t.handle);
        t = Texture{};
    }

    WGPUTextureView SubresourceView(const ViewRef &ref)
    {
        Texture &t = textures_[ref.texture];
        WGPUTextureView &view = t.views[ref.level << 16 | ref.layer];
        if (!view) {
            WGPUTextureViewDescriptor v = WGPU_TEXTURE_VIEW_DESCRIPTOR_INIT;
            v.dimension = WGPUTextureViewDimension_2D;
            v.baseMipLevel = ref.level;
            v.mipLevelCount = 1;
            v.baseArrayLayer = ref.layer;
            v.arrayLayerCount = 1;
            view = wgpuTextureCreateView(t.handle, &v);
        }
        return view;
    }

    Layout &LayoutFor(const uint8_t *slots)
    {
        std::string key((const char *)slots, kSamplers);
        auto it = layouts_.find(key);
        if (it != layouts_.end()) return it->second;
        WGPUBindGroupLayoutEntry entries[2 * kSamplers];
        uint32_t n = 0;
        for (uint32_t s = 0; s < kSamplers; ++s) {
            uint8_t slot = slots[s];
            if (!slot) continue;
            uint32_t binding = s < kPixelSamplers ? 2 * s : 32 + 2 * (s - kPixelSamplers);
            WGPUShaderStage stage = s < kPixelSamplers ? WGPUShaderStage_Fragment : WGPUShaderStage_Vertex;
            WGPUBindGroupLayoutEntry &tex = entries[n++] = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
            tex.binding = binding;
            tex.visibility = stage;
            tex.texture.sampleType = slot == kSlotDepth ? WGPUTextureSampleType_Depth : WGPUTextureSampleType_Float;
            tex.texture.viewDimension = slot == kSlotCube ? WGPUTextureViewDimension_Cube
                                        : slot == kSlot3D ? WGPUTextureViewDimension_3D
                                                          : WGPUTextureViewDimension_2D;
            WGPUBindGroupLayoutEntry &smp = entries[n++] = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
            smp.binding = binding + 1;
            smp.visibility = stage;
            smp.sampler.type = slot == kSlotDepth ? WGPUSamplerBindingType_Comparison : WGPUSamplerBindingType_Filtering;
        }
        Layout layout;
        WGPUBindGroupLayout groups[2] = {layout0_, nullptr};
        if (n) {
            WGPUBindGroupLayoutDescriptor d = WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT;
            d.entryCount = n;
            d.entries = entries;
            layout.group1 = groups[1] = wgpuDeviceCreateBindGroupLayout(device_, &d);
        }
        WGPUPipelineLayoutDescriptor d = WGPU_PIPELINE_LAYOUT_DESCRIPTOR_INIT;
        d.bindGroupLayoutCount = n ? 2 : 1;
        d.bindGroupLayouts = groups;
        layout.pipeline = wgpuDeviceCreatePipelineLayout(device_, &d);
        return layouts_.emplace(std::move(key), layout).first->second;
    }

    void CreatePipeline(const PipelineCmd &p)
    {
        Layout &layout = LayoutFor(p.slots);
        WGPUVertexAttribute attrs[kMaxAttrs];
        WGPUVertexBufferLayout buffers[kMaxStreams];
        uint32_t n = 0;
        for (uint32_t s = 0; s < p.streamCount; ++s) {
            buffers[s] = WGPU_VERTEX_BUFFER_LAYOUT_INIT;
            buffers[s].stepMode = WGPUVertexStepMode_Vertex;
            buffers[s].arrayStride = p.strides[s];
            buffers[s].attributes = attrs + n;
            for (uint32_t i = 0; i < p.attrCount; ++i) {
                if (p.attrs[i].stream != s) continue;
                attrs[n] = WGPU_VERTEX_ATTRIBUTE_INIT;
                attrs[n].format = VertexFormat(p.attrs[i].type);
                attrs[n].offset = p.attrs[i].offset;
                attrs[n].shaderLocation = p.attrs[i].location;
                ++n;
                ++buffers[s].attributeCount;
            }
        }
        WGPURenderPipelineDescriptor d = WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT;
        d.layout = layout.pipeline;
        d.vertex.module = shaders_[p.vs];
        d.vertex.entryPoint = View("main");
        d.vertex.bufferCount = p.streamCount;
        d.vertex.buffers = buffers;
        bool triangles = p.topology == D3DPT_TRIANGLELIST || p.topology == D3DPT_TRIANGLESTRIP;
        d.primitive.topology = Topology(p.topology);
        if (p.indexFormat) d.primitive.stripIndexFormat = IndexFormat(p.indexFormat);
        d.primitive.frontFace = WGPUFrontFace_CW;
        d.primitive.cullMode = p.cullMode == D3DCULL_CW    ? WGPUCullMode_Front
                               : p.cullMode == D3DCULL_CCW ? WGPUCullMode_Back
                                                           : WGPUCullMode_None;

        WGPUDepthStencilState ds = WGPU_DEPTH_STENCIL_STATE_INIT;
        if (p.depthFormat != TexFormat::None) {
            ds.format = TextureFormat(p.depthFormat);
            ds.depthWriteEnabled = p.zWrite ? WGPUOptionalBool_True : WGPUOptionalBool_False;
            ds.depthCompare = Compare(p.zFunc);
            ds.stencilFront = {Compare(p.stencilEnable ? p.stencilFunc : D3DCMP_ALWAYS), StencilOp(p.stencilFail),
                               StencilOp(p.stencilZFail), StencilOp(p.stencilPass)};
            ds.stencilBack = {Compare(p.stencilEnable ? p.ccwFunc : D3DCMP_ALWAYS), StencilOp(p.ccwFail),
                              StencilOp(p.ccwZFail), StencilOp(p.ccwPass)};
            ds.stencilReadMask = p.stencilEnable ? p.stencilMask : 0xFF;
            ds.stencilWriteMask = p.stencilEnable ? p.stencilWriteMask : 0;
            // Direct3D 9 biases in depth units; WebGPU in steps of the 24-bit resolution here.
            ds.depthBias = triangles ? (int32_t)std::lround(p.depthBias * 16777216.0f) : 0;
            ds.depthBiasSlopeScale = triangles ? p.slopeBias : 0.0f;
            d.depthStencil = &ds;
        }

        WGPUColorTargetState targets[kMaxTargets];
        WGPUBlendState blends[kMaxTargets];
        uint32_t targetCount = 0;
        for (uint32_t i = 0; i < kMaxTargets; ++i) {
            targets[i] = WGPU_COLOR_TARGET_STATE_INIT;
            if (p.colorFormats[i] == TexFormat::None) continue;
            targets[i].format = TextureFormat(p.colorFormats[i]);
            targets[i].writeMask = p.writeMasks[i];
            bool blendable = p.colorFormats[i] != TexFormat::R32F && p.colorFormats[i] != TexFormat::RG32F;
            if (p.blendEnable && blendable) {
                blends[i] = {Blend(p.blendOp, p.srcBlend, p.dstBlend),
                             Blend(p.blendOpAlpha, p.srcBlendAlpha, p.dstBlendAlpha)};
                targets[i].blend = &blends[i];
            }
            targetCount = i + 1;
        }
        WGPUFragmentState fragment = WGPU_FRAGMENT_STATE_INIT;
        fragment.module = shaders_[p.ps];
        fragment.entryPoint = View("main");
        fragment.targetCount = targetCount;
        fragment.targets = targets;
        d.fragment = &fragment;

        Pipeline &pipeline = Slot(pipelines_, p.id);
        pipeline.handle = wgpuDeviceCreateRenderPipeline(device_, &d);
        pipeline.streamCount = p.streamCount;
        pipeline.sampled = layout.group1 != nullptr;
    }

    WGPUSampler Sampler(const SamplerDesc &s, bool comparison)
    {
        std::string key((const char *)&s, sizeof s);
        key += comparison ? '1' : '0';
        WGPUSampler &sampler = samplers_[key];
        if (sampler) return sampler;
        WGPUSamplerDescriptor d = WGPU_SAMPLER_DESCRIPTOR_INIT;
        d.addressModeU = Address(s.addressU);
        d.addressModeV = Address(s.addressV);
        d.addressModeW = Address(s.addressW);
        bool anisotropic = s.minFilter == 3 || s.magFilter == 3;   // D3DTEXF_ANISOTROPIC
        d.maxAnisotropy = anisotropic && s.maxAnisotropy > 1 ? (uint16_t)std::min<uint32_t>(s.maxAnisotropy, 16) : 1;
        bool linear = d.maxAnisotropy > 1;
        d.magFilter = linear || s.magFilter >= 2 ? WGPUFilterMode_Linear : WGPUFilterMode_Nearest;
        d.minFilter = linear || s.minFilter >= 2 ? WGPUFilterMode_Linear : WGPUFilterMode_Nearest;
        d.mipmapFilter = linear || s.mipFilter == 2 ? WGPUMipmapFilterMode_Linear : WGPUMipmapFilterMode_Nearest;
        if (s.mipFilter == 0) d.lodMaxClamp = 0.0f;   // D3DTEXF_NONE: the top level only
        if (comparison) d.compare = WGPUCompareFunction_LessEqual;
        return sampler = wgpuDeviceCreateSampler(device_, &d);
    }

    void CreateBindings(const BindingsCmd &b)
    {
        Layout &layout = LayoutFor(b.slots);
        WGPUBindGroupEntry entries[2 * kSamplers];
        uint32_t n = 0;
        for (uint32_t s = 0; s < kSamplers; ++s) {
            uint8_t slot = b.slots[s];
            if (!slot) continue;
            uint32_t binding = s < kPixelSamplers ? 2 * s : 32 + 2 * (s - kPixelSamplers);
            uint32_t id = b.textures[s];
            bool bound = id && id < textures_.size() && textures_[id].sampled;
            entries[n] = WGPU_BIND_GROUP_ENTRY_INIT;
            entries[n].binding = binding;
            entries[n++].textureView = bound ? textures_[id].sampled : placeholders_[slot];
            entries[n] = WGPU_BIND_GROUP_ENTRY_INIT;
            entries[n].binding = binding + 1;
            entries[n++].sampler = Sampler(b.samplers[s], slot == kSlotDepth);
            if (bound) textures_[id].bindings.push_back(b.id);
        }
        WGPUBindGroupDescriptor d = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
        d.layout = layout.group1;
        d.entryCount = n;
        d.entries = entries;
        Slot(bindings_, b.id) = {wgpuDeviceCreateBindGroup(device_, &d), 0};
    }

    void DestroyBindings(const BindingsCmd &b)
    {
        Bindings &bindings = bindings_[b.id];
        wgpuBindGroupRelease(bindings.group);
        bindings = Bindings{};
    }

    void SetTargets(const TargetsCmd &t)
    {
        ResolveClear();
        EndPass();
        targets_ = t;
    }

    void Clear(const ClearCmd &c)
    {
        EndPass();
        if (c.flags & D3DCLEAR_TARGET) std::memcpy(clear_.color, c.color, sizeof c.color);
        if (c.flags & D3DCLEAR_ZBUFFER) clear_.depth = c.depth;
        if (c.flags & D3DCLEAR_STENCIL) clear_.stencil = c.stencil;
        clear_.flags |= c.flags;
    }

    // A clear with no draw after it still has to reach the targets.
    void ResolveClear()
    {
        if (clear_.flags && !pass_ && BeginPass()) EndPass();
        clear_.flags = 0;
    }

    bool BeginPass()
    {
        WGPURenderPassColorAttachment colors[kMaxTargets];
        uint32_t count = 0;
        for (uint32_t i = 0; i < kMaxTargets; ++i) {
            colors[i] = WGPU_RENDER_PASS_COLOR_ATTACHMENT_INIT;
            if (!targets_.colors[i].texture) continue;
            colors[i].view = SubresourceView(targets_.colors[i]);
            colors[i].loadOp = clear_.flags & D3DCLEAR_TARGET ? WGPULoadOp_Clear : WGPULoadOp_Load;
            colors[i].storeOp = WGPUStoreOp_Store;
            colors[i].clearValue = {clear_.color[0], clear_.color[1], clear_.color[2], clear_.color[3]};
            count = i + 1;
        }
        WGPURenderPassDepthStencilAttachment depth = WGPU_RENDER_PASS_DEPTH_STENCIL_ATTACHMENT_INIT;
        if (targets_.depth.texture) {
            depth.view = SubresourceView(targets_.depth);
            depth.depthLoadOp = clear_.flags & D3DCLEAR_ZBUFFER ? WGPULoadOp_Clear : WGPULoadOp_Load;
            depth.depthStoreOp = WGPUStoreOp_Store;
            depth.depthClearValue = clear_.depth;
            depth.stencilLoadOp = clear_.flags & D3DCLEAR_STENCIL ? WGPULoadOp_Clear : WGPULoadOp_Load;
            depth.stencilStoreOp = WGPUStoreOp_Store;
            depth.stencilClearValue = clear_.stencil;
        }
        if (!count && !targets_.depth.texture) return false;
        WGPURenderPassDescriptor d = WGPU_RENDER_PASS_DESCRIPTOR_INIT;
        d.colorAttachmentCount = count;
        d.colorAttachments = colors;
        d.depthStencilAttachment = targets_.depth.texture ? &depth : nullptr;
        pass_ = wgpuCommandEncoderBeginRenderPass(Encoder(), &d);
        clear_.flags = 0;
        passPipeline_ = nullptr;
        passGroup1_ = nullptr;
        passUniforms_[0] = ~0u;
        std::memset(passBuffers_, 0, sizeof passBuffers_);
        passIndex_ = nullptr;
        passViewport_[2] = -1;
        passScissor_[2] = -1;
        passStencilRef_ = ~0u;
        passBlendFactor_ = 0;   // a pass starts with a (0, 0, 0, 0) blend constant
        return true;
    }

    void EndPass()
    {
        if (!pass_) return;
        wgpuRenderPassEncoderEnd(pass_);
        wgpuRenderPassEncoderRelease(pass_);
        pass_ = nullptr;
    }

    void Draw(const DrawCmd &c)
    {
        Pipeline &p = pipelines_[c.pipeline];
        if (!p.handle || (!pass_ && !BeginPass())) return;
        Bindings *b = p.sampled && c.bindings < bindings_.size() ? &bindings_[c.bindings] : nullptr;
        if (p.sampled && (!b || !b->group)) return;
        if (passPipeline_ != p.handle) {
            wgpuRenderPassEncoderSetPipeline(pass_, p.handle);
            passPipeline_ = p.handle;
        }
        if (std::memcmp(passUniforms_, c.uniforms, sizeof passUniforms_)) {
            wgpuRenderPassEncoderSetBindGroup(pass_, 0, uniformGroup_, 3, c.uniforms);
            std::memcpy(passUniforms_, c.uniforms, sizeof passUniforms_);
        }
        segmentUsesUniforms_ = true;
        if (b) {
            if (passGroup1_ != b->group) {
                wgpuRenderPassEncoderSetBindGroup(pass_, 1, b->group, 0, nullptr);
                passGroup1_ = b->group;
            }
            b->segment = segment_;
        }
        for (uint32_t i = 0; i < p.streamCount; ++i) {
            Buffer &vb = buffers_[c.vertexBuffers[i]];
            if (passBuffers_[i].handle != vb.handle || passBuffers_[i].offset != c.vertexOffsets[i]) {
                wgpuRenderPassEncoderSetVertexBuffer(pass_, i, vb.handle, c.vertexOffsets[i], WGPU_WHOLE_SIZE);
                passBuffers_[i] = {vb.handle, c.vertexOffsets[i]};
            }
            vb.segment = segment_;
        }
        if (c.indexBuffer) {
            Buffer &ib = buffers_[c.indexBuffer];
            if (passIndex_ != ib.handle || passIndexFormat_ != c.indexFormat) {
                wgpuRenderPassEncoderSetIndexBuffer(pass_, ib.handle, IndexFormat(c.indexFormat), 0, WGPU_WHOLE_SIZE);
                passIndex_ = ib.handle;
                passIndexFormat_ = c.indexFormat;
            }
            ib.segment = segment_;
        }
        if (std::memcmp(passViewport_, c.viewport, sizeof passViewport_)) {
            wgpuRenderPassEncoderSetViewport(pass_, c.viewport[0], c.viewport[1], c.viewport[2], c.viewport[3],
                                             c.viewport[4], c.viewport[5]);
            std::memcpy(passViewport_, c.viewport, sizeof passViewport_);
        }
        if (std::memcmp(passScissor_, c.scissor, sizeof passScissor_)) {
            wgpuRenderPassEncoderSetScissorRect(pass_, c.scissor[0], c.scissor[1], c.scissor[2], c.scissor[3]);
            std::memcpy(passScissor_, c.scissor, sizeof passScissor_);
        }
        if (passStencilRef_ != c.stencilRef) {
            wgpuRenderPassEncoderSetStencilReference(pass_, c.stencilRef);
            passStencilRef_ = c.stencilRef;
        }
        if (passBlendFactor_ != c.blendFactor) {
            uint32_t f = c.blendFactor;
            WGPUColor color = {((f >> 16) & 0xFF) / 255.0, ((f >> 8) & 0xFF) / 255.0, (f & 0xFF) / 255.0,
                               ((f >> 24) & 0xFF) / 255.0};
            wgpuRenderPassEncoderSetBlendConstant(pass_, &color);
            passBlendFactor_ = c.blendFactor;
        }
        if (c.indexBuffer)
            wgpuRenderPassEncoderDrawIndexed(pass_, c.count, 1, c.first, c.baseVertex, 0);
        else
            wgpuRenderPassEncoderDraw(pass_, c.count, 1, c.first, 0);
    }

    WGPURenderPipeline BlitPipeline(WGPUTextureFormat format)
    {
        WGPURenderPipeline &pipeline = blitPipelines_[format];
        if (pipeline) return pipeline;
        WGPUColorTargetState target = WGPU_COLOR_TARGET_STATE_INIT;
        target.format = format;
        WGPUFragmentState fragment = WGPU_FRAGMENT_STATE_INIT;
        fragment.module = blitShader_;
        fragment.entryPoint = View("fs");
        fragment.targetCount = 1;
        fragment.targets = &target;
        WGPURenderPipelineDescriptor d = WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT;
        d.layout = blitLayout_;
        d.vertex.module = blitShader_;
        d.vertex.entryPoint = View("vs");
        d.fragment = &fragment;
        return pipeline = wgpuDeviceCreateRenderPipeline(device_, &d);
    }

    void Blit(WGPUTextureView source, WGPUTextureView target, WGPUTextureFormat format, const int32_t *rect,
              uint32_t targetWidth, uint32_t targetHeight, bool linear, uint32_t params)
    {
        WGPURenderPassColorAttachment color = WGPU_RENDER_PASS_COLOR_ATTACHMENT_INIT;
        color.view = target;
        color.loadOp = WGPULoadOp_Load;
        color.storeOp = WGPUStoreOp_Store;
        WGPURenderPassDescriptor d = WGPU_RENDER_PASS_DESCRIPTOR_INIT;
        d.colorAttachmentCount = 1;
        d.colorAttachments = &color;
        WGPURenderPassEncoder pass = wgpuCommandEncoderBeginRenderPass(Encoder(), &d);
        WGPUBindGroupEntry e[2] = {WGPU_BIND_GROUP_ENTRY_INIT, WGPU_BIND_GROUP_ENTRY_INIT};
        e[0].binding = 0;
        e[0].textureView = source;
        e[1].binding = 1;
        e[1].sampler = Sampler(SamplerDesc{3, 3, 3, linear ? 2u : 1u, linear ? 2u : 1u, 0, 1}, false);
        WGPUBindGroupDescriptor g = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
        g.layout = blitLayout1_;
        g.entryCount = 2;
        g.entries = e;
        WGPUBindGroup group = wgpuDeviceCreateBindGroup(device_, &g);
        wgpuRenderPassEncoderSetPipeline(pass, BlitPipeline(format));
        wgpuRenderPassEncoderSetBindGroup(pass, 0, blitGroup_, 1, &params);
        wgpuRenderPassEncoderSetBindGroup(pass, 1, group, 0, nullptr);
        uint32_t x = std::min<uint32_t>((uint32_t)std::max(rect[0], 0), targetWidth);
        uint32_t y = std::min<uint32_t>((uint32_t)std::max(rect[1], 0), targetHeight);
        wgpuRenderPassEncoderSetViewport(pass, (float)x, (float)y,
                                         (float)std::min<uint32_t>((uint32_t)std::max(rect[2], 0), targetWidth - x),
                                         (float)std::min<uint32_t>((uint32_t)std::max(rect[3], 0), targetHeight - y),
                                         0.0f, 1.0f);
        wgpuRenderPassEncoderDraw(pass, 3, 1, 0, 0);
        wgpuRenderPassEncoderEnd(pass);
        wgpuRenderPassEncoderRelease(pass);
        wgpuBindGroupRelease(group);
        segmentUsesUniforms_ = true;
    }

    void Copy(const CopyCmd &c)
    {
        ResolveClear();
        EndPass();
        Texture &src = textures_[c.src.texture], &dst = textures_[c.dst.texture];
        if (!src.handle || !dst.handle) return;
        Blit(SubresourceView(c.src), SubresourceView(c.dst), TextureFormat(dst.desc.format), c.dstRect,
             std::max(dst.desc.width >> c.dst.level, 1u), std::max(dst.desc.height >> c.dst.level, 1u), c.linear,
             c.params);
    }

    void Present(const PresentCmd &c)
    {
        ResolveClear();
        EndPass();
        WGPUSurfaceTexture frame = WGPU_SURFACE_TEXTURE_INIT;
        wgpuSurfaceGetCurrentTexture(surface_, &frame);
        if (frame.status == WGPUSurfaceGetCurrentTextureStatus_SuccessOptimal ||
            frame.status == WGPUSurfaceGetCurrentTextureStatus_SuccessSuboptimal) {
            WGPUTextureView view = wgpuTextureCreateView(frame.texture, nullptr);
            int32_t rect[4] = {0, 0, width_, height_};
            Blit(SubresourceView({c.backBuffer, 0, 0}), view, surfaceFormat_, rect, (uint32_t)width_,
                 (uint32_t)height_, true, c.params);
            wgpuTextureViewRelease(view);
        }
        FlushSegment();
        if (frame.texture) wgpuTextureRelease(frame.texture);
    }

    int width_, height_;
    std::atomic<int> state_{0};
    std::atomic<uint32_t> completed_{0};
    WGPUInstance instance_ = nullptr;
    WGPUAdapter adapter_ = nullptr;
    WGPUDevice device_ = nullptr;
    WGPUQueue queue_ = nullptr;
    WGPUSurface surface_ = nullptr;
    WGPUTextureFormat surfaceFormat_ = WGPUTextureFormat_Undefined;

    std::vector<Buffer> buffers_;
    std::vector<Texture> textures_;
    std::vector<WGPUShaderModule> shaders_;
    std::vector<Pipeline> pipelines_;
    std::vector<Bindings> bindings_;
    std::map<std::string, Layout> layouts_;
    std::unordered_map<std::string, WGPUSampler> samplers_;
    WGPUTextureView placeholders_[kSlotDepth + 1] = {};
    std::vector<WGPUTexture> deadTextures_;
    std::vector<WGPUBuffer> deadBuffers_;

    WGPUBindGroupLayout layout0_ = nullptr;
    WGPUBuffer zeros_ = nullptr;
    WGPUBuffer uniforms_ = nullptr;
    size_t uniformCapacity_ = 0;
    WGPUBindGroup uniformGroup_ = nullptr;
    WGPUBindGroupLayout blitLayout0_ = nullptr, blitLayout1_ = nullptr;
    WGPUPipelineLayout blitLayout_ = nullptr;
    WGPUShaderModule blitShader_ = nullptr;
    WGPUBindGroup blitGroup_ = nullptr;
    std::unordered_map<int, WGPURenderPipeline> blitPipelines_;

    WGPUCommandEncoder encoder_ = nullptr;
    uint32_t segment_ = 1;
    bool segmentUsesUniforms_ = false;
    TargetsCmd targets_ = {};
    ClearCmd clear_ = {};
    WGPURenderPassEncoder pass_ = nullptr;
    WGPURenderPipeline passPipeline_ = nullptr;
    WGPUBindGroup passGroup1_ = nullptr;
    uint32_t passUniforms_[3] = {};
    struct {
        WGPUBuffer handle;
        uint32_t offset;
    } passBuffers_[kMaxStreams] = {};
    WGPUBuffer passIndex_ = nullptr;
    uint32_t passIndexFormat_ = 0;
    float passViewport_[6] = {};
    int32_t passScissor_[4] = {};
    uint32_t passStencilRef_ = 0, passBlendFactor_ = 0;
};

} // namespace

Executor *CreateWebGpuExecutor(int width, int height)
{
    auto *executor = new WebGpuExecutor(width, height);
    if (executor->Start())
        return executor;
    delete executor;
    return nullptr;
}

} // namespace webdx
