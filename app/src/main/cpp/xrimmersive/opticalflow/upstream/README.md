# FidelityFX optical-flow kernels

Source: https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/tree/v1.1.4/sdk/include/FidelityFX/gpu/opticalflow
Pinned commit: c6efa6bf7f2027b3ec94f28578bb5965eabb9e55 (v1.1.4).

The four `ffx_opticalflow_*.h` files are unmodified upstream files. MIT license;
see LICENSE.txt and their individual copyright headers.

GameNative adapters, pyramid preparation and image synthesis are in the parent
directory. This is not AMD's complete frame-interpolation/FSR integration:
scene-change histograms are replaced by local consistency/color rejection,
three luma pyramid levels are used, and forward/backward flow is computed.
The search workgroup reductions are implemented using shared memory, independent
of subgroup size. Packed SAD uses the upstream GLSL scalar fallback, not msad4.
