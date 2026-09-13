# Hybrid Vulkan 1.21.1 native checkpoint

Pair this `feature/hybrid-vulkan-1.21.1` branch with the same branch in Polkadoty/Radiance. It consolidates the native source used by Hybrid6: DLSS/frame-generation recovery, DH terrain coverage and water integration support, persistent scene geometry, material caching, and Windows OpenGL/Vulkan shared-HUD transport.

See the Java repository's `HYBRID-STATUS.md` for the tested scope, remaining migration work, and companion/shaderpack sources. The backend is still hybrid; native Vulkan presentation does not mean every mod draw has been ported, or that arbitrary OpenGL geometry participates in ray tracing. Flywheel remains disabled in the tested instance.

Hybrid8 adds explicit overlay vertex layouts through ShaderProxy.registerShaderWithLayout. The boundary validates byte offsets/stride, attribute locations and GPU format support. UIModule owns a copy of each explicit layout and Pipeline.recreate carries it forward across resource reloads and resize. Pair with Radiance Hybrid8/native-hud.6; an older core does not export this entry point. Known enum layouts retain their existing route.

The local MSVC Release build and real-device startup registration/cache/invalid-layout probes passed. Full-pack reload/visual verification is tracked in Radiance's `docs/hybrid/HYBRID8-VALIDATION.md`. Native offscreen targets and GL-produced minimap textures still require compatibility transport. This work does not port Flywheel or arbitrary world geometry.

The source checkpoint was runtime-tested before consolidation. A clean-clone build and full-pack runtime test have not been repeated for this branch. Vendor runtimes, generated headers, external libraries and compiled shaders remain required. The Windows target now explicitly links `opengl32`, matching the locally tested hybrid build's dependency. No DLLs or user test data are included.
