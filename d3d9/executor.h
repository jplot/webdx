// executor.h — replays the front end's command stream on a graphics API.
#pragma once

#include <cstdint>
#include <vector>

namespace webdx {

// What the GPU offers beyond the WebGPU core the front end relies on.
struct Features {
    bool bc = false;               // BC1-3 textures; without them DXT is decoded on upload
    bool depth32Stencil8 = false;  // without it D24FS8 is stored as depth24plus-stencil8
};

class Executor {
public:
    virtual ~Executor() = default;
    virtual Features GetFeatures() const = 0;
    // Takes the commands recorded since the last call and the uniforms their draws address;
    // runs them asynchronously, in order.
    virtual void Submit(std::vector<uint8_t> &&commands, std::vector<uint8_t> &&uniforms) = 0;
    // Submissions fully executed so far.
    virtual uint32_t Completed() const = 0;
};

// WebGPU on the page's main thread, drawing to #canvas. Blocks the calling (non-main) thread
// until the device exists; nullptr when the browser offers no usable adapter.
Executor *CreateWebGpuExecutor(int width, int height);

} // namespace webdx
