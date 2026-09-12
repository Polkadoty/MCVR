#pragma once
#include "common/singleton.hpp"
#include "core/render/chunks.hpp"
#include "dh_lod_decode.hpp"
#include "geometry_budget.hpp"
#include <map>
#include <optional>
#include <span>

class Framework;
namespace radiance::dh {
struct LodSection {
    uint64_t revision = 0;
    std::vector<std::shared_ptr<ChunkBuildData>> parts;
};
// Separate from the vanilla toroidal chunk grid. All published buffers are immutable.
class LodScene : public SharedObject<LodScene> {
public:
    explicit LodScene(std::shared_ptr<Framework> framework);
    void configure(size_t liveBytes, size_t queueBytes);
    bool selectionPublished(uint64_t epoch);
    void beginWorld(uint64_t epoch);
    bool enqueue(std::span<const uint8_t> packet);
    void select(uint64_t epoch, const std::vector<std::pair<uint64_t,uint64_t>>& wanted);
    void process(std::shared_ptr<vk::CommandBuffer> commandBuffer);
    std::vector<std::shared_ptr<LodSection>> snapshot();
    struct Stats { size_t queuedBytes, pendingSections, readySections, activeSections; uint64_t epoch; bool open; };
    Stats stats();
    void close();
private:
    struct Pending {
        uint64_t revision = 0;
        uint32_t partCount = 0;
        std::vector<std::vector<uint8_t>> packets;
        std::vector<std::shared_ptr<ChunkBuildData>> built;
        size_t sourceBytes = 0;
        size_t acceptedBytes = 0;
        std::array<int32_t,3> origin{};
        uint32_t width = 0;
    };
    void retain(const std::shared_ptr<LodSection>& section);
    void discardPending(Pending& pending);
    void clearLocked();
    void tryPublish();
    std::weak_ptr<Framework> framework_;
    std::recursive_mutex mutex_;
    uint64_t epoch_ = 0;
    bool open_ = false;
    size_t queuedBytes_ = 0;
    GeometryBudget geometryBudget_;
    bool geometryDeferred_ = false;
    std::map<uint64_t, Pending> pending_;
    std::map<uint64_t, std::shared_ptr<LodSection>> ready_;
    std::map<uint64_t, std::shared_ptr<LodSection>> active_;
    std::map<uint64_t, uint64_t> wanted_;
    // Resident topology and queued CPU bytes are bounded independently.
    static constexpr size_t maxSections = 2048;
    size_t maxQueuedBytes = 32 * 1024 * 1024;
    bool queueFits(size_t bytes) const { return bytes <= SIZE_MAX - queuedBytes_ && (maxQueuedBytes == 0 || (queuedBytes_ <= maxQueuedBytes && bytes <= maxQueuedBytes - queuedBytes_)); }
    static constexpr size_t maxSectionBytes = 8 * 1024 * 1024;
};
}
