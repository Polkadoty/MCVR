#include "lod_scene.hpp"
#include "dh_lod_pbr.hpp"
#include "core/render/render_framework.hpp"
#include <algorithm>
#include <iostream>

namespace radiance::dh {
LodScene::LodScene(std::shared_ptr<Framework> framework) : framework_(framework) {}
void LodScene::configure(size_t liveBytes, size_t queueBytes) {
    std::lock_guard lock(mutex_);
    geometryBudget_.configure(liveBytes); maxQueuedBytes = queueBytes;
    std::cout << "[Radiance DH] Source limits: live=" << liveBytes << ", queue=" << queueBytes << " bytes (0=unlimited)" << std::endl;
}
bool LodScene::selectionPublished(uint64_t epoch) {
    std::lock_guard lock(mutex_);
    if (!open_ || epoch != epoch_ || active_.size() != wanted_.size()) return false;
    for (const auto& [id, revision] : wanted_) {
        auto active = active_.find(id);
        if (active == active_.end() || active->second->revision != revision) return false;
    }
    return true;
}
void LodScene::retain(const std::shared_ptr<LodSection>& section) {
    if (auto framework = framework_.lock()) framework->frameResourceRetainer().retain(section);
}
void LodScene::discardPending(Pending& pending) {
    queuedBytes_ -= pending.sourceBytes;
    if (auto framework = framework_.lock())
        for (auto& part : pending.built) framework->frameResourceRetainer().retain(part);
}
void LodScene::clearLocked() {
    for (auto& [id, section] : ready_) retain(section);
    for (auto& [id, section] : active_) retain(section);
    for (auto& [id, pending] : pending_) discardPending(pending);
    ready_.clear(); active_.clear(); pending_.clear(); wanted_.clear(); queuedBytes_ = 0;
}
void LodScene::beginWorld(uint64_t epoch) {
    std::lock_guard lock(mutex_);
    if (open_ && epoch == epoch_) return;
    if (epoch <= epoch_) throw std::invalid_argument("LOD world epoch must increase");
    clearLocked(); epoch_ = epoch; open_ = true;
}
void LodScene::close() {
    std::lock_guard lock(mutex_);
    clearLocked(); open_ = false;
}
bool LodScene::enqueue(std::span<const uint8_t> packet) {
    // Structural validation before retaining any untrusted native-bound bytes.
    // Decode again when building; this initial validation has no Vulkan operations.
    auto mesh = decode(packet);
    std::lock_guard lock(mutex_);
    if (!open_ || mesh.worldEpoch != epoch_) return false;
    auto desired = wanted_.find(mesh.sectionId);
    if (desired == wanted_.end() || desired->second != mesh.revision) return false;
    auto complete = ready_.find(mesh.sectionId);
    if (complete != ready_.end() && complete->second->revision == mesh.revision) return true;
    auto it = pending_.find(mesh.sectionId);
    if (it != pending_.end() && it->second.revision != mesh.revision) {
        if (it->second.revision > mesh.revision) return false;
        discardPending(it->second); pending_.erase(it); it = pending_.end();
    }
    if (it == pending_.end()) {
        if (pending_.size() >= maxSections || !queueFits(packet.size())) return false;
        Pending pending;
        pending.revision = mesh.revision; pending.partCount = mesh.partCount;
        pending.origin = mesh.origin; pending.width = mesh.sectionWidth;
        pending.packets.resize(mesh.partCount); pending.built.resize(mesh.partCount);
        it = pending_.emplace(mesh.sectionId, std::move(pending)).first;
    }
    auto& pending = it->second;
    if (pending.partCount != mesh.partCount) throw std::invalid_argument("LOD part count changed within revision");
    if (pending.origin != mesh.origin || pending.width != mesh.sectionWidth)
        throw std::invalid_argument("LOD origin or width changed within revision");
    if (pending.built[mesh.partIndex] || !pending.packets[mesh.partIndex].empty()) return true;
    if (packet.size() > maxSectionBytes - pending.acceptedBytes)
        throw std::invalid_argument("DH section exceeds the 8 MiB experimental bridge limit");
    if (!queueFits(packet.size())) return false;
    pending.packets[mesh.partIndex].assign(packet.begin(), packet.end());
    queuedBytes_ += packet.size(); pending.sourceBytes += packet.size();
    pending.acceptedBytes += packet.size();
    return true;
}
void LodScene::select(uint64_t epoch, const std::vector<std::pair<uint64_t,uint64_t>>& wanted) {
    std::lock_guard lock(mutex_);
    if (!open_ || epoch != epoch_) return;
    if (wanted.size() > maxSections) throw std::invalid_argument("Too many selected DH sections");
    std::map<uint64_t,uint64_t> next(wanted.begin(), wanted.end());
    if (next.size() != wanted.size()) throw std::invalid_argument("Duplicate selected DH section");
    wanted_ = std::move(next);
    for (auto it = pending_.begin(); it != pending_.end();) {
        auto wantedIt = wanted_.find(it->first);
        if (wantedIt == wanted_.end() || wantedIt->second != it->second.revision) {
            discardPending(it->second); it = pending_.erase(it);
        } else ++it;
    }
    for (auto it = ready_.begin(); it != ready_.end();) {
        auto desired = wanted_.find(it->first);
        if (desired == wanted_.end() || desired->second != it->second->revision) {
            // Old active coverage owns its own reference. Retire unpublished obsolete revisions
            // instead of letting them occupy the replacement budget indefinitely.
            retain(it->second); it = ready_.erase(it);
        }
        else ++it;
    }
    tryPublish();
}
void LodScene::tryPublish() {
    // Hold the old complete selection until all new parents/children are ready.
    // This avoids removing children while a replacement parent BLAS is still uploading.
    for (const auto& [id, revision] : wanted_) {
        auto ready = ready_.find(id);
        if (ready == ready_.end() || ready->second->revision != revision) return;
    }
    std::map<uint64_t,std::shared_ptr<LodSection>> next;
    for (const auto& [id, revision] : wanted_) next.emplace(id, ready_.at(id));
    if (next == active_) return;
    for (auto& [id, section] : active_) retain(section);
    active_ = std::move(next);
    size_t partCount = 0, vertexCount = 0;
    for (const auto& [id, section] : active_) {
        partCount += section->parts.size();
        for (const auto& part : section->parts) vertexCount += part->allVertexCount;
    }
    std::cout << "[Radiance DH] Published " << active_.size() << " sections, " << partCount
              << " parts, " << vertexCount << " vertices; epoch=" << epoch_ << std::endl;
}
#ifndef RADIANCE_DH_POLICY_TEST
void LodScene::process(std::shared_ptr<vk::CommandBuffer> commandBuffer) {
    std::lock_guard lock(mutex_);
    auto framework = framework_.lock();
    if (!open_ || !framework) return;
    // One <=2 MiB DH VBO per frame. No waitDeviceIdle or synchronous queue submit.
    for (auto it = pending_.begin(); it != pending_.end(); ++it) {
        auto& pending = it->second;
        for (size_t partIndex = 0; partIndex < pending.packets.size(); ++partIndex) {
            auto& bytes = pending.packets[partIndex];
            if (bytes.empty()) continue;
            auto mesh = decode(bytes);
            const size_t sourceBytes = mesh.vertices.size() * 16;
            // The companion limits a desired selection to half the live-geometry budget,
            // leaving room to replace the old coverage. Fence-retained objects count too.
            if (!geometryBudget_.canFit(sourceBytes)) {
                if (!geometryDeferred_) std::cout << "[Radiance DH] Deferring geometry upload: live-source-bytes="
                    << geometryBudget_.used() << ", requested=" << sourceBytes << "; preserving published coverage" << std::endl;
                geometryDeferred_ = true;
                return; // Keep the packet queued; old in-flight frames can retire next frame.
            }
            geometryDeferred_ = false;
            std::vector<vk::VertexFormat::PBRVertex> vertices;
            vertices.reserve(mesh.vertices.size());
            for (auto& vertex : mesh.vertices) vertices.push_back(toPbr(vertex, mesh.water));
            // Capture sizes before the argument list moves these vectors (argument order is unspecified).
            const auto vertexCount = static_cast<uint32_t>(vertices.size());
            const auto indexCount = static_cast<uint32_t>(mesh.indices.size());
            auto build = ChunkBuildData::create(static_cast<int64_t>(mesh.sectionId),
                mesh.origin[0], mesh.origin[1], mesh.origin[2], static_cast<int64_t>(mesh.revision),
                false, vertexCount, indexCount, vertexCount == 0 ? 0u : 1u,
                // Non-opaque BLAS flag is required so our near-coverage any-hit shaders execute.
                std::vector<World::GeometryTypes>{World::WORLD_TRANSPARENT}, std::vector<std::string>{mesh.water ? "radiance_dh_water" : "radiance_dh"},
                std::vector<std::vector<vk::VertexFormat::PBRVertex>>{std::move(vertices)},
                std::vector<std::vector<uint32_t>>{std::move(mesh.indices)});
            build->build(false);
            if (build->blas) {
                for (const auto& buffer : {build->positionBuffer, build->materialBuffer, build->indexBuffer})
                    buffer->uploadToBuffer(commandBuffer);
                commandBuffer->barriersMemory({vk::CommandBuffer::MemoryBarrier{
                    .srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                    .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
                    .dstStageMask = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR |
                                    VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR,
                    .dstAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR | VK_ACCESS_2_SHADER_READ_BIT}});
                std::vector<std::shared_ptr<vk::BLASBuilder>> builders{build->blasBuilder};
                vk::BLASBuilder::batchSubmit(builders, commandBuffer);
                framework->frameResourceRetainer().retain(build->blasBuilder);
                build->blasBuilder = nullptr;
            }
            build->vertices.clear(); build->indices.clear();
            geometryBudget_.track(build, sourceBytes);
            framework->frameResourceRetainer().retain(build);
            pending.built[partIndex] = build;
            queuedBytes_ -= bytes.size(); pending.sourceBytes -= bytes.size();
            bytes.clear(); bytes.shrink_to_fit();
            if (std::all_of(pending.built.begin(), pending.built.end(), [](const auto& part) { return part != nullptr; })) {
                auto section = std::make_shared<LodSection>();
                section->revision = pending.revision; section->parts = std::move(pending.built);
                if (auto old = ready_.find(it->first); old != ready_.end()) retain(old->second);
                ready_[it->first] = std::move(section); pending_.erase(it);
                tryPublish();
            }
            return;
        }
    }
}
#endif
LodScene::Stats LodScene::stats() {
    std::lock_guard lock(mutex_);
    return {queuedBytes_, pending_.size(), ready_.size(), active_.size(), epoch_, open_};
}
std::vector<std::shared_ptr<LodSection>> LodScene::snapshot() {
    std::lock_guard lock(mutex_);
    std::vector<std::shared_ptr<LodSection>> result;
    for (auto& [id, section] : active_) { retain(section); result.push_back(section); }
    return result;
}
}
