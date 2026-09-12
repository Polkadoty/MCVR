#pragma once
#include "dh_lod_decode.hpp"
#include "common/shared.hpp"

namespace radiance::dh {
// Compile-checked against MCVR's real PBRVertex layout. Not yet wired to BLAS storage.
inline vk::VertexFormat::PBRVertex toPbr(const Vertex& v, bool water = false) {
    vk::VertexFormat::PBRVertex out{};
    out.pos = {v.position[0], v.position[1], v.position[2]};
    out.useNorm = 1;
    out.norm = {v.normal[0], v.normal[1], v.normal[2]};
    out.useColorLayer = 1;
    out.colorLayer = {v.color[0], v.color[1], v.color[2], water ? v.color[3] : 1.f};
    out.alphaMode = water ? 2u : 0u; // ALPHA_MODE_TRANSPARENT / OPAQUE
    // Dedicated water hit group supplies IOR/Fresnel without a fake atlas texture.
    // Requires DH lodShading=DISABLED so these colors have no baked cardinal shading.
    // No lightmap multiplication: native path tracing computes illumination.
    // No terrain atlas sampling: DH's averaged face color is our initial albedo.
    // Block material and textureTile remain in the decoded mesh for future shading.
    return out;
}
}
