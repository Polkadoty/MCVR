#pragma once
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <vector>

// Experimental DH 3.2.0 CPU mesh ingestion. No Vulkan calls or global renderer state.
namespace radiance::dh {
struct Vertex {
    std::array<float, 3> position; // section-local; origin must be applied ONCE by instance transform
    std::array<float, 3> normal;
    std::array<float, 4> color; // encoded sRGB; DH lodShading must be DISABLED before mesh generation
    uint8_t skyLight, blockLight, material;
    uint16_t textureTile; // preserved for a future atlas path; flat color used initially
};
struct Mesh {
    uint64_t worldEpoch, sectionId, revision;
    std::array<int32_t, 3> origin;
    uint32_t sectionWidth;
    uint32_t partIndex, partCount;
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
    bool water = false; // packet flag1: all vertices are actual DH WATER material12
};
inline uint16_t u16(std::span<const uint8_t> b, size_t p) {
    return uint16_t(b[p]) | (uint16_t(b[p + 1]) << 8);
}
inline uint32_t u32(std::span<const uint8_t> b, size_t p) {
    return uint32_t(u16(b, p)) | (uint32_t(u16(b, p + 2)) << 16);
}
inline uint64_t u64(std::span<const uint8_t> b, size_t p) {
    return uint64_t(u32(b, p)) | (uint64_t(u32(b, p + 4)) << 32);
}
inline float offset(unsigned bits, float scale) {
    return (bits & 1) ? ((bits & 2) ? -scale : scale) : 0.f;
}

inline Mesh decode(std::span<const uint8_t> bytes, float microOffset = 0.f) {
    if (bytes.size() < 80 || u32(bytes, 0) != 0x444f4c52 || u32(bytes, 4) != 1
        || u32(bytes, 8) != 80 || (u32(bytes, 12) != 0 && u32(bytes, 12) != 1 && u32(bytes, 12) != 3) || u32(bytes, 60) != 0 || u64(bytes, 72) != 0)
        throw std::invalid_argument("Unsupported or truncated Radiance LOD packet");
    const uint32_t count = u32(bytes, 56), width = u32(bytes, 52);
    const uint32_t partIndex = u32(bytes, 64), partCount = u32(bytes, 68);
    if (count % 4 != 0 || count > 131072 || bytes.size() != 80 + size_t(count) * 16
        || width < 64 || width > 32768 || !std::has_single_bit(width)
        || partCount < 1 || partCount > 64 || partIndex >= partCount
        || !std::isfinite(microOffset) || microOffset < 0.f || microOffset > 0.01f)
        throw std::invalid_argument("Invalid LOD length, width or micro offset");
    Mesh result{u64(bytes, 16), u64(bytes, 24), u64(bytes, 32),
        {std::bit_cast<int32_t>(u32(bytes, 40)), std::bit_cast<int32_t>(u32(bytes, 44)),
         std::bit_cast<int32_t>(u32(bytes, 48))}, width, partIndex, partCount, {}, {}};
    result.water = (u32(bytes, 12) & 1) != 0;
    const bool markedSurface = u32(bytes, 12) == 3;
    result.vertices.reserve(count);
    result.indices.reserve(size_t(count) / 4 * 6);
    constexpr std::array<std::array<float, 3>, 6> normals{{
        {0,-1,0}, {0,1,0}, {0,0,-1}, {0,0,1}, {-1,0,0}, {1,0,0}}};
    for (uint32_t i = 0; i < count; ++i) {
        const size_t p = 80 + size_t(i) * 16;
        const unsigned x = u16(bytes, p), y = u16(bytes, p + 2), z = u16(bytes, p + 4);
        const unsigned meta = u16(bytes, p + 6), normal = bytes[p + 13];
        if (normal >= normals.size() || x > width || z > width
            || (!result.water && bytes[p + 11] != 255)
            || (result.water && (bytes[p + 12] != 12 || bytes[p + 11] == 0))
            || (markedSurface && (meta & 0x8000))
            || (markedSurface && (meta & 0x4000) && (normal == 0 || y == 0)))
            throw std::invalid_argument("Invalid DH vertex or material/packet mismatch");
        result.vertices.push_back(Vertex{
            {float(x) + offset(meta >> 8, microOffset), ((markedSurface && (meta & 0x4000)) ? float(y - 1) + (8.f / 9.f - 0.001f) : float(y)),
             float(z) + offset(meta >> 12, microOffset)}, normals[normal],
            {bytes[p+8]/255.f, bytes[p+9]/255.f, bytes[p+10]/255.f, bytes[p+11]/255.f},
            uint8_t(meta & 15), uint8_t((meta >> 4) & 15), bytes[p+12], u16(bytes, p+14)});
    }
    for (uint32_t base = 0; base < count; base += 4) {
        // Match DH's two triangles. Validate one coherent planar face per quad.
        const auto n = result.vertices[base].normal;
        for (uint32_t j = 1; j < 4; ++j)
            if (result.vertices[base+j].normal != n
                || result.vertices[base+j].material != result.vertices[base].material)
                throw std::invalid_argument("Mismatched DH quad normals");
        for (uint32_t i : {0u, 1u, 2u, 2u, 3u, 0u}) result.indices.push_back(base + i);
    }
    return result;
}
}
