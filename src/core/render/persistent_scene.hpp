#pragma once
#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <span>
#include <vector>

// CPU-only contract; wire packets use little endian scalar fields, never C++ struct layout.
namespace persistent {
struct Affine {
    std::array<float, 9> linear{1, 0, 0, 0, 1, 0, 0, 0, 1}; // row major
    std::array<double, 3> translation{};                    // absolute world coordinates
    std::array<float, 12> relativeTo(const std::array<double, 3> &camera) const;
    void validate() const;
};
struct Material {
    uint64_t id{}, revision{};
    uint32_t texture{}, alphaMode{}; // 0 opaque, 1 cutout; blending deliberately unsupported
    float emission{};
    std::shared_ptr<void> textureLease; // registered Vulkan image, retained through submitted frames
};
struct Vertex {
    std::array<float, 3> position, normal;
    std::array<float, 2> uv;
    std::array<float, 4> color;
};
struct Mesh {
    uint64_t id{}, revision{};
    std::shared_ptr<const Material> material;
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
    std::array<float, 3> min{}, max{};
    // Only the render thread reads/writes this immutable mesh's GPU cache.
    mutable std::shared_ptr<void> gpu;
};
struct Instance {
    uint64_t id{};
    std::shared_ptr<const Mesh> mesh;
    Affine transform;
};
struct Draw {
    Instance current;
    Affine previous;
    std::array<double, 3> previousCamera{};
    bool history{};
};
struct Frame {
    uint64_t epoch{}, serial{};
    std::array<double, 3> camera{};
    std::vector<Draw> draws;
};
class Scene {
  public:
    uint64_t reset();
    void material(uint64_t epoch, Material value);
    void mesh(uint64_t epoch,
              uint64_t id,
              uint64_t revision,
              uint64_t materialId,
              uint64_t materialRevision,
              std::span<const std::byte> packet);
    // Replaces the entire visible set atomically. Missing instances immediately lose history.
    void instances(uint64_t epoch, std::span<const std::byte> packet);
    void retireMesh(uint64_t epoch, uint64_t id);
    void retireMaterial(uint64_t epoch, uint64_t id);
    std::shared_ptr<Frame> snapshot(const std::array<double, 3> &camera);
    uint64_t epoch() const;

  private:
    void check(uint64_t epoch) const;
    mutable std::mutex mutex_;
    uint64_t epoch_{1}, serial_{};
    size_t bytes_{};
    std::map<uint64_t, std::shared_ptr<const Material>> materials_;
    std::map<uint64_t, std::shared_ptr<const Mesh>> meshes_;
    std::map<uint64_t, uint64_t> materialRevisions_, meshRevisions_;
    std::map<uint64_t, Instance> visible_;
    std::shared_ptr<Frame> previous_;
};
} // namespace persistent
