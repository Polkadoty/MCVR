#pragma once
#include "core/render/entities.hpp"
#include "core/render/persistent_scene.hpp"
namespace persistent {
struct GpuMesh {
    std::shared_ptr<EntityBuildDataBatch> batch;
    std::shared_ptr<EntityBuildData> data;
};
// Render thread only. Builds each immutable mesh once, before TLAS construction.
std::shared_ptr<Frame> prepare(Scene &scene,
                               const std::array<double, 3> &camera,
                               std::shared_ptr<Framework> framework,
                               std::shared_ptr<vk::CommandBuffer> command);
} // namespace persistent
