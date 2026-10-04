# Native OpenXR session for the Meta Quest "immersive launch" mode.
#
# Built against the official Khronos OpenXR loader (org.khronos.openxr:openxr_loader_for_android,
# Apache-2.0, pulled in as a Gradle/prefab dependency in app/build.gradle.kts) — no code or
# binaries from any third-party Winlator VR fork are used here.
#
# NOT wired into the normal Gradle build (see the other commented-out externalNativeBuild
# blocks in app/build.gradle.kts) — this project ships native libs as prebuilt .so files
# committed to jniLibs/, rebuilt manually when needed. To build this module locally:
#   1. Temporarily uncomment the externalNativeBuild { cmake { path = ... } } block for
#      this CMakeLists.txt in app/build.gradle.kts.
#   2. Build, then copy the resulting libxrimmersive.so into
#      app/src/main/jniLibs/arm64-v8a/ and re-comment the block.
cmake_minimum_required(VERSION 3.22.1)
project(xrimmersive)
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)

add_library(xrimmersive SHARED
    xr_vulkan_dispatch.cpp
    xr_vulkan_context.cpp
    xr_present_foveation.cpp
    xr_vulkan_present.cpp
    xr_immersive.cpp
    jni_bridge.cpp
    xr_windows_projection.cpp
    xr_sgsr.cpp
    xr_windows_transport.cpp
)

# Temporary output-path diagnostic; never enabled by normal builds.
option(GN_XR_SWAPCHAIN_PATTERN "Replace VR content with a swapchain test pattern" OFF)
if(GN_XR_SWAPCHAIN_PATTERN)
    target_compile_definitions(xrimmersive PRIVATE GN_XR_SWAPCHAIN_PATTERN=1)
endif()

find_package(OpenXR REQUIRED CONFIG)

target_link_libraries(xrimmersive
    OpenXR::openxr_loader
    EGL
    GLESv3
    android
    jnigraphics
    log
)

target_compile_definitions(xrimmersive PRIVATE
    XR_USE_TIMESPEC=1
    XR_USE_PLATFORM_ANDROID=1
    XR_USE_GRAPHICS_API_VULKAN=1
    XR_USE_GRAPHICS_API_OPENGL_ES=1
    EGL_EGLEXT_PROTOTYPES=1
    GL_GLEXT_PROTOTYPES=1
)

target_compile_options(xrimmersive PRIVATE -Wall -Wextra -Werror=return-type -Wno-missing-field-initializers)

# Reuse the native LSFG compute backend, with a private dispatch table. The XR
# device must never overwrite the flat renderer's Vulkan device dispatch.
set(LSFG_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../lsfg)
target_sources(xrimmersive PRIVATE xr_lsfg.cpp xr_lsfg_capture.cpp xr_optical_flow.cpp
    ${LSFG_DIR}/vk_dispatch.c ${LSFG_DIR}/lsfg_dll.c
    ${LSFG_DIR}/lsfg_common.cpp ${LSFG_DIR}/lsfg_shaders.cpp
    ${LSFG_DIR}/lsfg_mipmaps.cpp ${LSFG_DIR}/lsfg_alpha.cpp
    ${LSFG_DIR}/lsfg_beta.cpp ${LSFG_DIR}/lsfg_gamma.cpp
    ${LSFG_DIR}/lsfg_delta.cpp ${LSFG_DIR}/lsfg_generate.cpp
    ${LSFG_DIR}/lsfg_chain.cpp)
target_compile_features(xrimmersive PRIVATE cxx_std_17)
target_compile_definitions(xrimmersive PRIVATE LSFG_CACHE_ONLY=1 LSFG_STEREO_2X=1
    VK_USE_PLATFORM_ANDROID_KHR=1 vkd=xr_lsfg_dispatch
    vkd_load=xr_lsfg_load_dispatch vkd_unload=xr_lsfg_unload_dispatch)
set_target_properties(xrimmersive PROPERTIES C_VISIBILITY_PRESET hidden
    CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN YES)
target_link_libraries(xrimmersive vulkan)
