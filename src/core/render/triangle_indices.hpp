#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace entity_geometry {

// Match triangle assembly in the source draw topology. Incomplete final
// primitives are ignored, just as in the source graphics API.
inline void appendTriangleListIndices(std::vector<uint32_t> &indices, uint32_t vertexCount) {
    const uint32_t completeVertices = vertexCount - vertexCount % 3;
    indices.reserve(indices.size() + completeVertices);
    for (uint32_t i = 0; i < completeVertices; ++i) { indices.push_back(i); }
}

inline void appendTriangleStripIndices(std::vector<uint32_t> &indices, uint32_t vertexCount) {
    if (vertexCount < 3) return;
    indices.reserve(indices.size() + static_cast<std::size_t>(vertexCount - 2) * 3);
    for (uint32_t i = 2; i < vertexCount; ++i) {
        // A strip reverses the first two vertices of every other triangle,
        // retaining consistent face winding without reading a future vertex.
        indices.push_back((i & 1) ? i - 1 : i - 2);
        indices.push_back((i & 1) ? i - 2 : i - 1);
        indices.push_back(i);
    }
}

}  // namespace entity_geometry
