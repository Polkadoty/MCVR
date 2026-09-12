#include "core/render/modules/world/ray_tracing/submodules/world_prepare.hpp"

#include "core/render/buffers.hpp"
#include "core/render/chunks.hpp"
#include "core/render/compat/dh/lod_scene.hpp"
#include "core/render/compat/dh/near_coverage.hpp"
#include "core/render/entities.hpp"
#include "core/render/modules/world/ray_tracing/ray_tracing_module.hpp"
#include "core/render/render_framework.hpp"
#include "core/render/renderer.hpp"
#include "core/render/world.hpp"
#include "core/render/persistent_scene_gpu.hpp"

#include <filesystem>
#include <fstream>
#include <chrono>
#include <glm/gtc/type_ptr.hpp>

WorldPrepare::WorldPrepare() {}

void WorldPrepare::init(std::shared_ptr<Framework> framework, std::shared_ptr<RayTracingModule> rayTracingModule) {
    framework_ = framework;
    rayTracingModule_ = rayTracingModule;
}

void WorldPrepare::build() {
    auto framework = framework_.lock();
    auto rayTracingModule = rayTracingModule_.lock();
    uint32_t size = framework->swapchain()->imageCount();

    contexts_.resize(size);

    for (int i = 0; i < size; i++) {
        contexts_[i] = WorldPrepareContext::create(framework->contexts()[i], shared_from_this());
    }
}

WorldPrepareContext::WorldPrepareContext(std::shared_ptr<FrameworkContext> frameworkContext,
                                         std::shared_ptr<WorldPrepare> worldPrepare)
    : frameworkContext(frameworkContext), worldPrepare(worldPrepare) {}

void WorldPrepareContext::uploadBuffer(std::vector<uint32_t> &blasOffsets,
                                       std::vector<uint64_t> &indexBufferAddrs,
                                       std::vector<uint64_t> &positionBufferAddrs,
                                       std::vector<uint64_t> &materialBufferAddrs,
                                       std::vector<uint64_t> &lastIndexBufferAddrs,
                                       std::vector<uint64_t> &lastPositionBufferAddrs,
                                       std::vector<glm::mat4> &lastObjToWorldMats) {
    auto context = frameworkContext.lock();
    auto framework = context->framework.lock();
    auto vma = framework->vma();
    auto device = framework->device();
    auto physicalDevice = framework->physicalDevice();
    auto mainQueueIndex = physicalDevice->mainQueueIndex();
    auto cmdBuffer = context->worldCommandBuffer;

    blasOffsetsBuffer = vk::DeviceLocalBuffer::create(
        vma, device, blasOffsets.size() * sizeof(uint32_t),
        VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
            VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    blasOffsetsBuffer->uploadToStagingBuffer(blasOffsets.data());

    indexBufferAddr = vk::DeviceLocalBuffer::create(
        vma, device, indexBufferAddrs.size() * sizeof(uint64_t),
        VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
            VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    indexBufferAddr->uploadToStagingBuffer(indexBufferAddrs.data());

    positionBufferAddr = vk::DeviceLocalBuffer::create(
        vma, device, positionBufferAddrs.size() * sizeof(uint64_t),
        VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
            VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    positionBufferAddr->uploadToStagingBuffer(positionBufferAddrs.data());

    materialBufferAddr = vk::DeviceLocalBuffer::create(
        vma, device, materialBufferAddrs.size() * sizeof(uint64_t),
        VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
            VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    materialBufferAddr->uploadToStagingBuffer(materialBufferAddrs.data());

    lastIndexBufferAddr = vk::DeviceLocalBuffer::create(
        vma, device, lastIndexBufferAddrs.size() * sizeof(uint64_t),
        VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
            VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    lastIndexBufferAddr->uploadToStagingBuffer(lastIndexBufferAddrs.data());

    lastPositionBufferAddr = vk::DeviceLocalBuffer::create(
        vma, device, lastPositionBufferAddrs.size() * sizeof(uint64_t),
        VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
            VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    lastPositionBufferAddr->uploadToStagingBuffer(lastPositionBufferAddrs.data());

    lastObjToWorldMat = vk::DeviceLocalBuffer::create(
        vma, device, lastObjToWorldMats.size() * sizeof(glm::mat4),
        VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
            VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    lastObjToWorldMat->uploadToStagingBuffer(lastObjToWorldMats.data());

    std::vector<std::shared_ptr<vk::DeviceLocalBuffer>> rayTracingMetaData{{
        dhNearCoverageBuffer,
        blasOffsetsBuffer,
        indexBufferAddr,
        positionBufferAddr,
        materialBufferAddr,
        lastIndexBufferAddr,
        lastPositionBufferAddr,
        lastObjToWorldMat,
    }};

    std::vector<vk::CommandBuffer::BufferMemoryBarrier> uploadPreBufferBarriers, uploadPostBufferBarriers;

    for (auto buffer : rayTracingMetaData) {
        if (buffer == nullptr) continue;
        uploadPreBufferBarriers.push_back({
            .srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
            .srcAccessMask = VK_ACCESS_2_MEMORY_READ_BIT,
            .dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
            .dstAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT,
            .srcQueueFamilyIndex = mainQueueIndex,
            .dstQueueFamilyIndex = mainQueueIndex,
            .buffer = buffer,
        });
        uploadPostBufferBarriers.push_back({
            .srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
            .srcAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
            .dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT |
                            VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR |
                            VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR,
            .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
            .srcQueueFamilyIndex = mainQueueIndex,
            .dstQueueFamilyIndex = mainQueueIndex,
            .buffer = buffer,
        });
    }

    cmdBuffer->barriersBufferImage(uploadPreBufferBarriers, {});
    for (auto buffer : rayTracingMetaData) {
        if (buffer == nullptr) continue;
        buffer->uploadToBuffer(cmdBuffer);
    }
    cmdBuffer->barriersBufferImage(uploadPostBufferBarriers, {});
}

void WorldPrepareContext::render() {
    auto rayTracingContext = rayTracingModuleContext.lock();
    auto rayTracingModule = rayTracingContext != nullptr ? rayTracingContext->rayTracingModule.lock() : nullptr;
    auto worldPrepare1 = worldPrepare.lock();
    if (rayTracingModule == nullptr) { return; }
    if (worldPrepare1 == nullptr) { return; }

    std::shared_ptr<Framework> framework = Renderer::instance().framework();
    std::shared_ptr<FrameworkContext> context = frameworkContext.lock();
    std::shared_ptr<vk::VMA> vma = framework->vma();
    std::shared_ptr<vk::Device> device = framework->device();
    std::shared_ptr<vk::PhysicalDevice> physicalDevice = framework->physicalDevice();
    std::shared_ptr<vk::CommandBuffer> worldCommandBuffer = context->worldCommandBuffer;

    auto chunks = Renderer::instance().world()->chunks();
    auto entities = Renderer::instance().world()->entities();
    auto cameraPos = Renderer::instance().world()->getCameraPos();

    auto chunkBuildScheduler = chunks->chunkBuildScheduler();
    if (chunkBuildScheduler != nullptr) {
        chunkBuildScheduler->tryCheckBatchesFinish();
        chunkBuildScheduler->tryScheduleBatches(chunkBuildScheduler->chunkBuildingBatchSize());
    }

    std::unique_lock<std::recursive_mutex> lock(chunks->mutex());

    if (chunks->importantBLASBuilders().size() > 0) {
        vk::BLASBuilder::batchSubmit(chunks->importantBLASBuilders(), worldCommandBuffer);
    }

    if (entities->blasBatchBuilder() != nullptr) { entities->blasBatchBuilder()->submit(worldCommandBuffer); }

    auto lods = Renderer::instance().world()->lods();
    lods->process(worldCommandBuffer);

    worldCommandBuffer->barriersMemory({vk::CommandBuffer::MemoryBarrier{
        .srcStageMask = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
        .srcAccessMask =
            VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR | VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR,
        .dstStageMask = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
        .dstAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR,
    }});

    uint32_t blasAccu = 0, blasGroupAccu = 0;
    std::vector<uint32_t> blasOffset;
    hitGroupNames.clear();
    std::vector<uint64_t> indexBufferAddrs;
    std::vector<uint64_t> positionBufferAddrs, materialBufferAddrs;
    std::vector<uint64_t> lastIndexBufferAddrs;
    std::vector<uint64_t> lastPositionBufferAddrs;
    std::vector<glm::mat4> lastObjToWorldMats;

    tlasBuilder = vk::TLASBuilder::create();
    auto &instanceBuilder = tlasBuilder->beginInstanceBuilder();
    int blasIndex = 0;

    // Entity
    {
        auto entityBatch = entities->entityBatch();

        if (entityBatch != nullptr) {
            std::unique_lock<std::recursive_mutex> entityHistoryLock(worldPrepare1->entityRenderDataBatchesMtx_);
            auto &previousEntityRenderDataBatches = worldPrepare1->previousEntityRenderDataBatches_;
            auto &emptyEntityRenderDataBatch = worldPrepare1->emptyEntityRenderDataBatch_;

            auto &previousEntityRenderDataBatch = previousEntityRenderDataBatches.empty() ?
                                                      emptyEntityRenderDataBatch :
                                                      previousEntityRenderDataBatches.back();
            if (previousEntityRenderDataBatches.size() > Renderer::instance().framework()->swapchain()->imageCount())
                previousEntityRenderDataBatches.pop();
            auto &currentEntityRenderDataBatch = previousEntityRenderDataBatches.emplace();

            auto worldUniformBuffer = Renderer::instance().buffers()->worldUniformBuffer();
            auto ubo = static_cast<vk::Data::WorldUBO *>(worldUniformBuffer->mappedPtr());

            auto &entities1 = entityBatch->entities;
            for (int i = 0; i < entities1.size(); i++) {
                VkGeometryInstanceFlagsKHR flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
                // VkGeometryInstanceFlagsKHR flags = 0;
                VkTransformMatrixKHR transform;

                if (entities1[i]->prebuiltBLAS < 0) {
                    if (entities1[i]->coordinate == World::Coordinates::WORLD || !ubo) {
                        transform = {
                            1, 0, 0, static_cast<float>(entities1[i]->x - cameraPos.x), //
                            0, 1, 0, static_cast<float>(entities1[i]->y - cameraPos.y), //
                            0, 0, 1, static_cast<float>(entities1[i]->z - cameraPos.z), //
                        };
                    } else if (entities1[i]->coordinate == World::Coordinates::CAMERA) {
                        glm::mat4 viewMat = glm::transpose(ubo->cameraViewMatInv); // column major to row major

                        transform = {
                            viewMat[0][0], viewMat[0][1], viewMat[0][2], viewMat[0][3], //
                            viewMat[1][0], viewMat[1][1], viewMat[1][2], viewMat[1][3], //
                            viewMat[2][0], viewMat[2][1], viewMat[2][2], viewMat[2][3], //
                        };
                    } else if (entities1[i]->coordinate == World::Coordinates::CAMERA_SHIFT) {
                        glm::vec3 shift = glm::vec3(ubo->cameraViewMatInv[3]);
                        transform = {
                            1, 0, 0, shift.x, //
                            0, 1, 0, shift.y, //
                            0, 0, 1, shift.z, //
                        };
                    }

                    instanceBuilder.defineInstance(transform, blasIndex, entities1[i]->rayTracingFlag, blasGroupAccu,
                                                   flags, entities1[i]->blas);
                } else {
                    // auto &prebuiltBLAS =
                    //     Renderer::instance().framework()->prebuiltBLASs()[entityRenderData->prebuiltBLAS];
                    // transform = prebuiltBLAS.align(*entityRenderData->vertices, *entityRenderData->indices);

                    // instanceBuilder.defineInstance(transform, blasIndex, entityRenderData->rayTracingFlag,
                    // blasGroupAccu, flags,
                    //                                prebuiltBLAS.blas);
                    throw std::runtime_error("prebuilt blas not implemented yet!");
                }

                hitGroupNames.push_back("shadow");
                for (int j = 0; j < entities1[i]->geometryCount; j++) {
                    const std::string &groupName =
                        entities1[i]->geometryGroupNames != nullptr &&
                                j < static_cast<int>(entities1[i]->geometryGroupNames->size()) ?
                            (*entities1[i]->geometryGroupNames)[j] :
                            "default";
                    hitGroupNames.push_back(groupName);
                }

                for (int j = 0; j < entities1[i]->geometryCount; j++) {
                    indexBufferAddrs.push_back((*entities1[i]->indexBufferAddresses)[j]);
                    positionBufferAddrs.push_back((*entities1[i]->positionBufferAddresses)[j]);
                    materialBufferAddrs.push_back((*entities1[i]->materialBufferAddresses)[j]);
                }

                {
                    if (entities1[i]->hashCode) {
                        currentEntityRenderDataBatch[entities1[i]->hashCode].first = entities1[i];
                        currentEntityRenderDataBatch[entities1[i]->hashCode].second = transform;
                    }
                }

                {
                    glm::mat4 lastObjToWorldMat(1);
                    auto iter = previousEntityRenderDataBatch.find(entities1[i]->hashCode);
                    if (iter != previousEntityRenderDataBatch.end()) {
                        auto &previousEntityRenderData = (*iter).second.first;
                        if (previousEntityRenderData->geometryCount == entities1[i]->geometryCount) {
                            for (int j = 0; j < entities1[i]->geometryCount; j++) {
                                if ((*previousEntityRenderData->vertexCounts)[j] == (*entities1[i]->vertexCounts)[j] &&
                                    (*previousEntityRenderData->indexCounts)[j] == (*entities1[i]->indexCounts)[j]) {
                                    lastIndexBufferAddrs.push_back(
                                        (*previousEntityRenderData->indexBufferAddresses)[j]);
                                    lastPositionBufferAddrs.push_back(
                                        (*previousEntityRenderData->positionBufferAddresses)[j]);
                                } else {
                                    lastIndexBufferAddrs.push_back(0);
                                    lastPositionBufferAddrs.push_back(0);
                                }
                            }
                        } else {
                            for (int j = 0; j < entities1[i]->geometryCount; j++) {
                                lastIndexBufferAddrs.push_back(0);
                                lastPositionBufferAddrs.push_back(0);
                            }
                        }

                        VkTransformMatrixKHR lastObjToWorldVkMat = iter->second.second;
                        lastObjToWorldMat = glm::transpose(glm::mat4(glm::make_vec4(lastObjToWorldVkMat.matrix[0]), //
                                                                     glm::make_vec4(lastObjToWorldVkMat.matrix[1]), //
                                                                     glm::make_vec4(lastObjToWorldVkMat.matrix[2]), //
                                                                     glm::vec4(0.0f, 0.0f, 0.0f, 1.0f)));
                    } else {
                        for (int j = 0; j < entities1[i]->geometryCount; j++) {
                            lastIndexBufferAddrs.push_back(0);
                            lastPositionBufferAddrs.push_back(0);
                        }
                    }
                    lastObjToWorldMats.push_back(lastObjToWorldMat);
                }

                blasOffset.push_back(blasAccu);
                blasAccu += entities1[i]->geometryCount;
                blasGroupAccu += entities1[i]->geometryCount + 1;

                blasIndex++;
            }
        }
    }

    // Persistent reusable geometry. Separate IDs/history from transient entity hashes.
    // prepare() records mesh copies and BLAS builds before this frame's TLAS build.
    auto persistentFrame = persistent::prepare(Renderer::instance().world()->persistentScene(),
        {cameraPos.x, cameraPos.y, cameraPos.z}, framework, worldCommandBuffer);
    for (const auto& draw : persistentFrame->draws) {
        const auto gpu = std::static_pointer_cast<persistent::GpuMesh>(draw.current.mesh->gpu);
        const auto current = draw.current.transform.relativeTo(persistentFrame->camera);
        VkTransformMatrixKHR transform{};
        std::memcpy(transform.matrix, current.data(), sizeof(transform.matrix));
        instanceBuilder.defineInstance(transform, blasIndex, 0x01, blasGroupAccu,
            VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR, gpu->data->blas);
        hitGroupNames.push_back("shadow"); hitGroupNames.push_back("default");
        indexBufferAddrs.push_back(gpu->data->indexBufferAddresses[0]);
        positionBufferAddrs.push_back(gpu->data->positionBufferAddresses[0]);
        materialBufferAddrs.push_back(gpu->data->materialBufferAddresses[0]);
        lastIndexBufferAddrs.push_back(draw.history ? gpu->data->indexBufferAddresses[0] : 0);
        lastPositionBufferAddrs.push_back(draw.history ? gpu->data->positionBufferAddresses[0] : 0);
        const auto previous = draw.previous.relativeTo(draw.previousCamera);
        glm::mat4 previousMatrix(1.0f);
        for (int r=0; r<3; ++r) for (int c=0; c<4; ++c) previousMatrix[c][r]=previous[r*4+c];
        lastObjToWorldMats.push_back(previousMatrix);
        blasOffset.push_back(blasAccu++); blasGroupAccu += 2; ++blasIndex;
    }
    // Same-frame ready coverage: only completed normal sections suppress overlapping LODs.
    // Coordinate tags protect against toroidal slot reuse while flying/teleporting.
    const auto grid = chunks->chunkGridInfo();
    auto coverage = radiance::dh::emptyCoverage({grid.x, grid.y, grid.z, grid.w});
    size_t readyCount = 0, emptyCount = 0, collisions = 0, invalidBlas = 0;
    for (const auto& chunk : chunks->chunks()) {
        if (!chunk->terrainReady) { invalidBlas += chunk->blas != nullptr; continue; }
        ++readyCount; emptyCount += chunk->blas == nullptr;
        const int x = chunk->x / 16, y = chunk->y / 16, z = chunk->z / 16;
        if (grid.x > 0 && grid.z > 0 && y >= grid.w && y < grid.w + grid.y) {
            const auto& old = coverage[radiance::dh::coverageIndex(coverage[0], x, y, z)];
            if (old[3] && old != radiance::dh::CoverageCell{x,y,z,1}) ++collisions;
        }
        radiance::dh::markCoverage(coverage, x, y, z, true);
    }
    // Bounded test diagnostics: exact CPU publication used by this frame's TLAS.
    // No world/database data; one overwritten terrain-readiness snapshot every 5 seconds.
    static auto nextCoverageReport = std::chrono::steady_clock::time_point{};
    const auto reportNow = std::chrono::steady_clock::now();
    if (reportNow >= nextCoverageReport) {
        nextCoverageReport = reportNow + std::chrono::seconds(5);
        std::ofstream report("radiance/dh-coverage.csv", std::ios::trunc);
        if (report) {
            report << "camera," << cameraPos.x << ',' << cameraPos.y << ',' << cameraPos.z << '\n'
                   << "grid," << grid.x << ',' << grid.y << ',' << grid.z << ',' << grid.w << '\n'
                   << "ready,empty,collisions,blasWithoutReady\n" << readyCount << ',' << emptyCount
                   << ',' << collisions << ',' << invalidBlas << "\nx,y,z,ready\n";
            for (size_t i=1; i<coverage.size(); ++i) if (coverage[i][3])
                report << coverage[i][0] << ',' << coverage[i][1] << ',' << coverage[i][2] << ",1\n";
        }
    }
    dhNearCoverageBuffer = vk::DeviceLocalBuffer::create(vma, device,
        coverage.size() * sizeof(radiance::dh::CoverageCell), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    dhNearCoverageBuffer->uploadToStagingBuffer(coverage.data());

    // Chunk
    {
        auto &chunk1s = chunks->chunks();
        for (int i = 0; i < chunk1s.size(); i++) {
            auto &chunk1 = chunk1s[i];
            if (chunk1->blas == nullptr) continue;
            chunk1->retainResources(framework->frameResourceRetainer());

            VkTransformMatrixKHR transform = {
                1, 0, 0, static_cast<float>(static_cast<double>(chunk1->x) - cameraPos.x), //
                0, 1, 0, static_cast<float>(static_cast<double>(chunk1->y) - cameraPos.y), //
                0, 0, 1, static_cast<float>(static_cast<double>(chunk1->z) - cameraPos.z), //
            };

            instanceBuilder.defineInstance(transform, blasIndex, 0x01, blasGroupAccu, 0, chunk1->blas);

            hitGroupNames.push_back("shadow");
            for (int j = 0; j < chunk1->geometryCount; j++) {
                const std::string &groupName =
                    chunk1->geometryGroupNames != nullptr && j < static_cast<int>(chunk1->geometryGroupNames->size()) ?
                        (*chunk1->geometryGroupNames)[j] :
                        "default";
                hitGroupNames.push_back(groupName);
            }

            for (int j = 0; j < chunk1->geometryCount; j++) {
                indexBufferAddrs.push_back((*chunk1->indexBufferAddresses)[j]);
                positionBufferAddrs.push_back((*chunk1->positionBufferAddresses)[j]);
                materialBufferAddrs.push_back((*chunk1->materialBufferAddresses)[j]);
                lastIndexBufferAddrs.push_back(0);
                lastPositionBufferAddrs.push_back(0);
            }

            {
                glm::mat4 lastObjToWorldMat = glm::transpose(glm::mat4(
                    glm::vec4(1.0f, 0.0f, 0.0f, static_cast<float>(static_cast<double>(chunk1->x) - cameraPos.x)), //
                    glm::vec4(0.0f, 1.0f, 0.0f, static_cast<float>(static_cast<double>(chunk1->y) - cameraPos.y)), //
                    glm::vec4(0.0f, 0.0f, 1.0f, static_cast<float>(static_cast<double>(chunk1->z) - cameraPos.z)), //
                    glm::vec4(0.0f, 0.0f, 0.0f, 1.0f)));
                lastObjToWorldMats.push_back(lastObjToWorldMat);
            }

            blasOffset.push_back(blasAccu);
            blasAccu += chunk1->geometryCount;
            blasGroupAccu += chunk1->geometryCount + 1;

            blasIndex++;
        }
    }

    // Distant Horizons: independent immutable LOD BLASes, never vanilla chunk slot IDs.
    for (const auto& section : lods->snapshot()) {
        for (const auto& part : section->parts) {
            if (!part->blas) continue;
            VkTransformMatrixKHR transform = {
                1, 0, 0, static_cast<float>(static_cast<double>(part->x) - cameraPos.x),
                0, 1, 0, static_cast<float>(static_cast<double>(part->y) - cameraPos.y),
                0, 0, 1, static_cast<float>(static_cast<double>(part->z) - cameraPos.z)};
            instanceBuilder.defineInstance(transform, blasIndex, 0x01, blasGroupAccu, 0, part->blas);
            const bool dhWater = !part->geometryGroupNames.empty() && part->geometryGroupNames[0] == "radiance_dh_water";
            hitGroupNames.push_back(dhWater ? "radiance_dh_water_shadow" : "radiance_dh_shadow");
            for (uint32_t j = 0; j < part->geometryCount; ++j) {
                hitGroupNames.push_back(dhWater ? "radiance_dh_water" : "radiance_dh");
                indexBufferAddrs.push_back(part->indexBufferAddresses[j]);
                positionBufferAddrs.push_back(part->positionBufferAddresses[j]);
                materialBufferAddrs.push_back(part->materialBufferAddresses[j]);
                lastIndexBufferAddrs.push_back(0);
                lastPositionBufferAddrs.push_back(0);
            }
            lastObjToWorldMats.push_back(glm::transpose(glm::mat4(
                glm::vec4(1,0,0,transform.matrix[0][3]),
                glm::vec4(0,1,0,transform.matrix[1][3]),
                glm::vec4(0,0,1,transform.matrix[2][3]), glm::vec4(0,0,0,1))));
            blasOffset.push_back(blasAccu);
            blasAccu += part->geometryCount;
            blasGroupAccu += part->geometryCount + 1;
            ++blasIndex;
        }
    }

    if (instanceBuilder.instances.empty()) {
        tlas = nullptr;
        return;
    }

    tlas = instanceBuilder.endInstanceBuilder(device, vma)
               ->defineBuildProperty(VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR)
               ->querySizeInfo(device)
               ->allocateBuffers(physicalDevice, device, vma)
               ->buildAndSubmit(device, worldCommandBuffer);

    worldCommandBuffer->barriersMemory({vk::CommandBuffer::MemoryBarrier{
        .srcStageMask = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
        .srcAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR,
        .dstStageMask = VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR,
        .dstAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR,
    }});

    uploadBuffer(blasOffset, indexBufferAddrs, positionBufferAddrs, materialBufferAddrs, lastIndexBufferAddrs,
                 lastPositionBufferAddrs, lastObjToWorldMats);
}

void WorldPrepareContext::setupHitGroupSbt(const std::unordered_map<std::string, uint32_t> &hitGroupNameToIndex,
                                           uint32_t fallbackHitGroupIndex,
                                           uint32_t shadowHitGroupIndex,
                                           std::shared_ptr<vk::CommandBuffer> commandBuffer,
                                           std::shared_ptr<vk::SBT> updateSbt,
                                           std::shared_ptr<vk::SBT> querySbt) {
    std::vector<uint32_t> hitGroupIndices;
    hitGroupIndices.reserve(hitGroupNames.size());

    for (const std::string &groupName : hitGroupNames) {
        if (groupName == "shadow") {
            hitGroupIndices.push_back(shadowHitGroupIndex);
            continue;
        }

        auto iter = hitGroupNameToIndex.find(groupName);
        if (groupName.starts_with("radiance_dh") && iter == hitGroupNameToIndex.end())
            throw std::runtime_error("Active shaderpack lacks required Radiance DH hit groups");
        hitGroupIndices.push_back(iter == hitGroupNameToIndex.end() ? fallbackHitGroupIndex : iter->second);
    }

    if (updateSbt != nullptr) { updateSbt->setupHitSBT(hitGroupIndices, commandBuffer); }
    if (querySbt != nullptr) { querySbt->setupHitSBT(hitGroupIndices, commandBuffer); }
}
