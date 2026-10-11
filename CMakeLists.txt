cmake_minimum_required(VERSION 3.16)
project(webdx C CXX)

if (NOT TARGET webwin)
    message(FATAL_ERROR "webdx needs the webwin target: add_subdirectory(webwin) first")
endif()

add_library(webdx STATIC
    d3d9/glcontext_sdl.cpp
    d3d9/gl_d3d9.cpp
    d3d9/gl_d3d9_draw.cpp
    d3d9/gl_resources.cpp
    d3d9/gl_state.cpp
    d3d9/gl_shader.cpp
    d3d9/gl_program.cpp
    d3d9/gl_query.cpp
    d3d9/gl_format.cpp
    d3d9/gl_surface_ops.cpp
    d3d9/gl_d3dx.cpp
    d3d9/gl_stubs_web.cpp
    xaudio2/al_audio.cpp
    xaudio2/libwma/wmadeci.c
    xaudio2/libwma/wmafixed.c
    xaudio2/libwma/mdct.c
    xaudio2/libwma/mdct_lookup.c
    xaudio2/libwma/fft-ffmpeg.c
    xaudio2/libwma/ffmpeg_bitstream.c
    xaudio2/libwma/wma_decode.c
)
set_target_properties(webdx PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON C_STANDARD 11)

target_include_directories(webdx
    PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/d3d9" "${CMAKE_CURRENT_SOURCE_DIR}/xaudio2" "${CMAKE_CURRENT_SOURCE_DIR}/xaudio2/libwma"
)
target_link_libraries(webdx PUBLIC webwin)

if (EMSCRIPTEN)
    target_sources(webdx PRIVATE d3d9/device.cpp d3d9/format.cpp d3d9/sm3_wgsl.cpp d3d9/webgpu.cpp)
    target_compile_options(webdx PRIVATE --use-port=emdawnwebgpu)
    target_link_options(webdx INTERFACE --use-port=emdawnwebgpu)
    target_link_options(webdx INTERFACE
        -sMAX_WEBGL_VERSION=2
        -sMIN_WEBGL_VERSION=2
        -sFULL_ES3=1
        -lopenal
    )
    if (WEBWIN_PTHREADS)
        target_link_options(webdx INTERFACE
            -sOFFSCREEN_FRAMEBUFFER=1
            "SHELL:--post-js ${CMAKE_CURRENT_SOURCE_DIR}/web/kb_blit_patch.js"
        )
    endif()
else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(WEBDX_GLEW REQUIRED glew)
    find_package(OpenGL REQUIRED)
    find_library(WEBDX_OPENAL openal REQUIRED)
    target_include_directories(webdx PRIVATE ${WEBDX_GLEW_INCLUDE_DIRS})
    target_link_libraries(webdx PUBLIC ${WEBDX_GLEW_LIBRARIES} OpenGL::GL ${WEBDX_OPENAL})
endif()

option(WEBDX_TESTS "Build the native smoke tests (needs a desktop GL and OpenAL)" OFF)
if (WEBDX_TESTS AND NOT EMSCRIPTEN)
    file(GLOB WEBDX_GL_TESTS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/d3d9/tests/smoke_*.cpp")
    foreach (test IN LISTS WEBDX_GL_TESTS)
        get_filename_component(name "${test}" NAME_WE)
        add_executable(${name} "${test}")
        target_link_libraries(${name} PRIVATE webdx)
        target_include_directories(${name} PRIVATE ${WEBDX_GLEW_INCLUDE_DIRS})
    endforeach()
    add_executable(al_smoke xaudio2/tests/al_smoke.cpp)
    target_link_libraries(al_smoke PRIVATE webdx)
endif()

option(WEBDX_WEB_TESTS "Build the browser tests" OFF)
if (WEBDX_WEB_TESTS AND EMSCRIPTEN)
    add_executable(webgpu_clear tests/web/webgpu_clear.cpp)
    target_compile_options(webgpu_clear PRIVATE -pthread --use-port=emdawnwebgpu)
    target_link_options(webgpu_clear PRIVATE -pthread --use-port=emdawnwebgpu -sPROXY_TO_PTHREAD -sEXIT_RUNTIME=0)
    set_target_properties(webgpu_clear PROPERTIES SUFFIX ".html")

    add_executable(device_threads tests/web/device_threads.cpp d3d9/device.cpp d3d9/format.cpp d3d9/sm3_wgsl.cpp)
    set_target_properties(device_threads PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
    target_include_directories(device_threads PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/d3d9")
    target_link_libraries(device_threads PRIVATE webwin)
    target_include_directories(device_threads PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
    target_compile_options(device_threads PRIVATE -pthread)
    target_link_options(device_threads PRIVATE -pthread -sPROXY_TO_PTHREAD -sEXIT_RUNTIME=1 -sPTHREAD_POOL_SIZE=8
        -sINITIAL_MEMORY=512MB)
endif()
