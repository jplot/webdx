// Same threads as the engine: main() on a pthread (PROXY_TO_PTHREAD), WebGPU on the page's main
// thread, which presents each frame when it returns to its event loop.
#include <webgpu/webgpu.h>

#include <emscripten.h>
#include <emscripten/proxying.h>
#include <emscripten/threading.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <unistd.h>

namespace {

WGPUInstance g_instance;
WGPUAdapter  g_adapter;
WGPUDevice   g_device;
WGPUQueue    g_queue;
WGPUSurface  g_surface;
std::atomic<int> g_state{0};   // 0 pending, 1 ready, -1 failed
std::atomic<int> g_frames{0};

WGPUStringView View(const char *s) { return WGPUStringView{s, strlen(s)}; }

void OnDevice(WGPURequestDeviceStatus status, WGPUDevice device, WGPUStringView message, void *, void *)
{
    if (status != WGPURequestDeviceStatus_Success) {
        printf("[webgpu_clear] device failed: %.*s\n", (int)message.length, message.data);
        g_state = -1;
        return;
    }
    g_device = device;
    g_queue = wgpuDeviceGetQueue(device);

    WGPUEmscriptenSurfaceSourceCanvasHTMLSelector canvas = WGPU_EMSCRIPTEN_SURFACE_SOURCE_CANVAS_HTML_SELECTOR_INIT;
    canvas.selector = View("#canvas");
    WGPUSurfaceDescriptor surfaceDesc = WGPU_SURFACE_DESCRIPTOR_INIT;
    surfaceDesc.nextInChain = &canvas.chain;
    g_surface = wgpuInstanceCreateSurface(g_instance, &surfaceDesc);

    WGPUSurfaceCapabilities caps = WGPU_SURFACE_CAPABILITIES_INIT;
    wgpuSurfaceGetCapabilities(g_surface, g_adapter, &caps);
    WGPUSurfaceConfiguration config = WGPU_SURFACE_CONFIGURATION_INIT;
    config.device = device;
    config.format = caps.formats[0];
    config.width = 640;
    config.height = 360;
    wgpuSurfaceConfigure(g_surface, &config);
    printf("[webgpu_clear] device ready, surface format %d\n", (int)caps.formats[0]);
    wgpuSurfaceCapabilitiesFreeMembers(caps);
    g_state = 1;
}

void OnAdapter(WGPURequestAdapterStatus status, WGPUAdapter adapter, WGPUStringView message, void *, void *)
{
    if (status != WGPURequestAdapterStatus_Success) {
        printf("[webgpu_clear] adapter failed: %.*s\n", (int)message.length, message.data);
        g_state = -1;
        return;
    }
    g_adapter = adapter;
    WGPURequestDeviceCallbackInfo info = WGPU_REQUEST_DEVICE_CALLBACK_INFO_INIT;
    info.mode = WGPUCallbackMode_AllowSpontaneous;
    info.callback = OnDevice;
    wgpuAdapterRequestDevice(adapter, nullptr, info);
}

void InitOnMain(void *)
{
    g_instance = wgpuCreateInstance(nullptr);
    WGPURequestAdapterCallbackInfo info = WGPU_REQUEST_ADAPTER_CALLBACK_INFO_INIT;
    info.mode = WGPUCallbackMode_AllowSpontaneous;
    info.callback = OnAdapter;
    wgpuInstanceRequestAdapter(g_instance, nullptr, info);
}

void FrameOnMain(void *arg)
{
    const int frame = (int)(intptr_t)arg;
    WGPUSurfaceTexture target = WGPU_SURFACE_TEXTURE_INIT;
    wgpuSurfaceGetCurrentTexture(g_surface, &target);
    WGPUTextureView view = wgpuTextureCreateView(target.texture, nullptr);

    WGPURenderPassColorAttachment color = WGPU_RENDER_PASS_COLOR_ATTACHMENT_INIT;
    color.view = view;
    color.loadOp = WGPULoadOp_Clear;
    color.storeOp = WGPUStoreOp_Store;
    color.clearValue = WGPUColor{0.0, (frame % 120) / 120.0, 0.5, 1.0};
    WGPURenderPassDescriptor pass = WGPU_RENDER_PASS_DESCRIPTOR_INIT;
    pass.colorAttachmentCount = 1;
    pass.colorAttachments = &color;

    WGPUCommandEncoder encoder = wgpuDeviceCreateCommandEncoder(g_device, nullptr);
    WGPURenderPassEncoder render = wgpuCommandEncoderBeginRenderPass(encoder, &pass);
    wgpuRenderPassEncoderEnd(render);
    WGPUCommandBuffer commands = wgpuCommandEncoderFinish(encoder, nullptr);
    wgpuQueueSubmit(g_queue, 1, &commands);

    wgpuCommandBufferRelease(commands);
    wgpuRenderPassEncoderRelease(render);
    wgpuCommandEncoderRelease(encoder);
    wgpuTextureViewRelease(view);
    wgpuTextureRelease(target.texture);
    ++g_frames;
}

} // namespace

int main()
{
    em_proxying_queue *queue = emscripten_proxy_get_system_queue();
    pthread_t page = emscripten_main_runtime_thread_id();
    emscripten_proxy_async(queue, page, InitOnMain, nullptr);
    while (g_state == 0)
        usleep(1000);
    if (g_state < 0)
        return 1;
    for (int frame = 0; frame < 600; ++frame) {
        emscripten_proxy_async(queue, page, FrameOnMain, (void *)(intptr_t)frame);
        usleep(16000);
        if (frame % 60 == 59)
            printf("[webgpu_clear] submitted %d frames, executed %d\n", frame + 1, g_frames.load());
    }
    return 0;
}
