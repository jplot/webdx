# webdx

DirectX for C++ code compiled to WebAssembly with Emscripten: Direct3D 9 on WebGL 2 (shader model 3 bytecode translated to GLSL ES 3.00), XAudio2 on OpenAL with xWMA and MS-ADPCM decoding, and the D3DX, XInput, DirectSound and DirectDraw headers.

## Use

```cmake
add_subdirectory(webwin)   # webdx builds on webwin's Windows headers and types
add_subdirectory(webdx)
target_link_libraries(app PRIVATE webdx)
```

`Direct3DCreate9` and `XAudio2Create` are the entry points, as on Windows. Linking `webdx` adds the WebGL 2 and OpenAL link options; with `WEBWIN_PTHREADS` it also renders through an offscreen framebuffer presented on the page's `#canvas`.

## Origin

Extracted from the web port of [KisakBlack](https://github.com/SwagSoftware/KisakBlack) by [riicchhaarrd](https://github.com/riicchhaarrd/KisakBlack/tree/web-port). Licensed under the GPL-3.0, like KisakBlack. `xaudio2/libwma` is the WMA decoder of Rockbox and FFmpeg, whose files are under the LGPL-2.0-or-later, LGPL-2.1-or-later or GPL-2.0-or-later.
