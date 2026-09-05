#include "core/render/persistent_scene_gpu.hpp"
#include "core/render/render_framework.hpp"
#include "core/render/renderer.hpp"
#include "core/render/textures.hpp"
namespace persistent {
std::shared_ptr<Frame> prepare(Scene &scene,
                               const std::array<double, 3> &camera,
                               std::shared_ptr<Framework> framework,
                               std::shared_ptr<vk::CommandBuffer> command) {
    auto frame = scene.snapshot(camera);
    // Retain before recording, including when a later allocation/recording operation throws.
    framework->frameResourceRetainer().retain(frame);
    bool built = false;
    for (auto &draw : frame->draws) {
        const auto &mesh = *draw.current.mesh;
        if (mesh.gpu) continue;
        auto gpu = std::make_shared<GpuMesh>();
        std::vector<vk::VertexFormat::PBRVertex> vertices;
        vertices.reserve(mesh.vertices.size());
        for (const auto &src : mesh.vertices) {
            vk::VertexFormat::PBRVertex v{};
            v.pos = {src.position[0], src.position[1], src.position[2]};
            v.norm = {src.normal[0], src.normal[1], src.normal[2]};
            v.useNorm = 1;
            v.colorLayer = {src.color[0], src.color[1], src.color[2], src.color[3]};
            v.useColorLayer = 1;
            v.textureUV = {src.uv[0], src.uv[1]};
            v.textureID = mesh.material->texture;
            v.useTexture = 1;
            v.alphaMode = mesh.material->alphaMode;
            v.albedoEmission = mesh.material->emission;
            v.coordinate = World::WORLD;
            vertices.push_back(v);
        }
        std::vector<std::vector<vk::VertexFormat::PBRVertex>> geometry;
        geometry.push_back(std::move(vertices));
        std::vector<std::vector<uint32_t>> indices{mesh.indices};
        gpu->data = std::make_shared<EntityBuildData>(
            0, 0., 0., 0., 1, 0, -1, World::WORLD, 1,
            std::vector<World::GeometryTypes>{mesh.material->alphaMode == 0 ? World::WORLD_SOLID :
                                                                              World::WORLD_TRANSPARENT},
            std::vector<std::string>{"default"}, std::vector<std::string>{"persistent-mesh"}, std::move(geometry),
            std::move(indices));
        gpu->batch = EntityBuildDataBatch::create();
        gpu->batch->addData(gpu->data);
        gpu->batch->build();
        // WorldPrepare runs after the framework upload pass. Record these copies HERE,
        // not in queueImportantWorldUpload(), which would miss this frame's upload pass.
        mesh.gpu = gpu;
        gpu->batch->positionBuffer->uploadToBuffer(command);
        gpu->batch->materialBuffer->uploadToBuffer(command);
        gpu->batch->indexBuffer->uploadToBuffer(command);
        command->barriersMemory({vk::CommandBuffer::MemoryBarrier{
            .srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
            .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
            .dstStageMask = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR |
                            VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR,
            .dstAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR | VK_ACCESS_2_SHADER_READ_BIT}});
        gpu->batch->blasBatchBuilder->submit(command);
        framework->frameResourceRetainer().retain(gpu->batch->blasBatchBuilder);
        gpu->batch->blasBatchBuilder.reset(); // scratch lives until frame fence, not mesh lifetime
        gpu->data->vertices.clear();
        gpu->data->indices.clear();
        built = true;
    }
    if (built)
        command->barriersMemory(
            {vk::CommandBuffer::MemoryBarrier{.srcStageMask = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                                              .srcAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR,
                                              .dstStageMask = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                                              .dstAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR}});
    return frame;
}
} // namespace persistent
