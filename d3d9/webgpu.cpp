// webgpu.cpp — the WebGPU executor: owns the device and #canvas on the page's main thread, which
// presents each frame when it returns to its event loop.
#include "executor.h"
#include "stream.h"

#include <webgpu/webgpu.h>

#include <emscripten.h>
#include <emscripten/html5.h>
#include <emscripten/proxying.h>
#include <emscripten/threading.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <unistd.h>

namespace webdx {
namespace {

WGPUStringView View(const char *s) { return WGPUStringView{s, strlen(s)}; }

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

    void Submit(std::vector<uint8_t> &&commands) override
    {
        auto *job = new Job{this, std::move(commands)};
        emscripten_proxy_async(emscripten_proxy_get_system_queue(), emscripten_main_runtime_thread_id(),
                               [](void *p) {
                                   Job *job = static_cast<Job *>(p);
                                   job->executor->Run(job->commands);
                                   job->executor->completed_.fetch_add(1);
                                   delete job;
                               },
                               job);
    }

    uint32_t Completed() const override { return completed_.load(); }

private:
    struct Job {
        WebGpuExecutor *executor;
        std::vector<uint8_t> commands;
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
        WGPURequestDeviceCallbackInfo info = WGPU_REQUEST_DEVICE_CALLBACK_INFO_INIT;
        info.mode = WGPUCallbackMode_AllowSpontaneous;
        info.userdata1 = this;
        info.callback = [](WGPURequestDeviceStatus status, WGPUDevice device, WGPUStringView message, void *self, void *) {
            static_cast<WebGpuExecutor *>(self)->OnDevice(status, device, message);
        };
        wgpuAdapterRequestDevice(adapter, nullptr, info);
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
        WGPUSurfaceConfiguration config = WGPU_SURFACE_CONFIGURATION_INIT;
        config.device = device;
        config.format = caps.formats[0];
        config.width = (uint32_t)width_;
        config.height = (uint32_t)height_;
        wgpuSurfaceConfigure(surface_, &config);
        wgpuSurfaceCapabilitiesFreeMembers(caps);
        fprintf(stderr, "[webdx] WebGPU device ready (%dx%d)\n", width_, height_);
        state_ = 1;
    }

    void Run(const std::vector<uint8_t> &commands)
    {
        StreamReader reader(commands.data(), commands.size());
        Cmd cmd;
        const uint8_t *payload;
        uint32_t size;
        while (reader.Next(cmd, payload, size)) {
            switch (cmd) {
            case Cmd::Clear: clear_ = StreamReader::Read<ClearCmd>(payload); break;
            case Cmd::Present: Present(); break;
            }
        }
    }

    void Present()
    {
        WGPUSurfaceTexture target = WGPU_SURFACE_TEXTURE_INIT;
        wgpuSurfaceGetCurrentTexture(surface_, &target);
        WGPUTextureView view = wgpuTextureCreateView(target.texture, nullptr);
        WGPURenderPassColorAttachment color = WGPU_RENDER_PASS_COLOR_ATTACHMENT_INIT;
        color.view = view;
        color.loadOp = WGPULoadOp_Clear;
        color.storeOp = WGPUStoreOp_Store;
        color.clearValue = WGPUColor{clear_.color[0], clear_.color[1], clear_.color[2], clear_.color[3]};
        WGPURenderPassDescriptor pass = WGPU_RENDER_PASS_DESCRIPTOR_INIT;
        pass.colorAttachmentCount = 1;
        pass.colorAttachments = &color;
        WGPUCommandEncoder encoder = wgpuDeviceCreateCommandEncoder(device_, nullptr);
        WGPURenderPassEncoder render = wgpuCommandEncoderBeginRenderPass(encoder, &pass);
        wgpuRenderPassEncoderEnd(render);
        WGPUCommandBuffer commands = wgpuCommandEncoderFinish(encoder, nullptr);
        wgpuQueueSubmit(queue_, 1, &commands);
        wgpuCommandBufferRelease(commands);
        wgpuRenderPassEncoderRelease(render);
        wgpuCommandEncoderRelease(encoder);
        wgpuTextureViewRelease(view);
        wgpuTextureRelease(target.texture);
    }

    int width_, height_;
    std::atomic<int> state_{0};
    std::atomic<uint32_t> completed_{0};
    WGPUInstance instance_ = nullptr;
    WGPUAdapter adapter_ = nullptr;
    WGPUDevice device_ = nullptr;
    WGPUQueue queue_ = nullptr;
    WGPUSurface surface_ = nullptr;
    ClearCmd clear_ = {{0, 0, 0, 1}};
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
