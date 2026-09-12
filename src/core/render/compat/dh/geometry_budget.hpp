#pragma once
#include <algorithm>
#include <cstddef>
#include <memory>
#include <limits>
#include <vector>

namespace radiance::dh {
// Counts live geometry, including old frames retained by Vulkan fences. Does not own it.
class GeometryBudget {
public:
    static constexpr size_t limit = 32 * 1024 * 1024;
    void configure(size_t bytes) { limit_ = bytes; }
    size_t used() {
        std::erase_if(entries_, [](const Entry& e) { return e.object.expired(); });
        size_t bytes = 0;
        for (const auto& entry : entries_) bytes += entry.bytes;
        return bytes;
    }
    bool canFit(size_t bytes) { const auto live = used(); return bytes <= std::numeric_limits<size_t>::max() - live && (limit_ == 0 || (bytes <= limit_ && live <= limit_ - bytes)); }
    void track(const std::shared_ptr<void>& object, size_t bytes) {
        if (bytes != 0) entries_.push_back({object, bytes});
    }
private:
    size_t limit_ = limit;
    struct Entry { std::weak_ptr<void> object; size_t bytes; };
    std::vector<Entry> entries_;
};
}
