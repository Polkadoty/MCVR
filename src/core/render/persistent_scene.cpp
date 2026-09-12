#include "core/render/persistent_scene.hpp"
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>

namespace persistent {
namespace {
    constexpr size_t budget = 256ull * 1024 * 1024;
    constexpr uint32_t magicMesh = 0x314d5052, magicInstances = 0x31495052; // RPM1, RPI1
    void require(bool b, const char *message) {
        if (!b) throw std::invalid_argument(message);
    }
    struct Reader {
        std::span<const std::byte> data;
        size_t at{};
        template <class T>
        T get() {
            static_assert(std::endian::native == std::endian::little);
            require(sizeof(T) <= data.size() - at, "truncated persistent scene packet");
            T result;
            std::memcpy(&result, data.data() + at, sizeof(T));
            at += sizeof(T);
            return result;
        }
        void end() {
            require(at == data.size(), "trailing persistent scene packet bytes");
        }
    };
    size_t meshBytes(const Mesh &m) {
        return m.vertices.size() * sizeof(Vertex) + m.indices.size() * 4;
    }
} // namespace
void Affine::validate() const {
    for (float f : linear) require(std::isfinite(f) && std::abs(f) <= 1e6f, "invalid affine linear component");
    for (double d : translation) require(std::isfinite(d) && std::abs(d) <= 1e12, "invalid world translation");
    const auto &a = linear;
    double det = double(a[0]) * (double(a[4]) * a[8] - double(a[5]) * a[7]) -
                 double(a[1]) * (double(a[3]) * a[8] - double(a[5]) * a[6]) +
                 double(a[2]) * (double(a[3]) * a[7] - double(a[4]) * a[6]);
    require(std::isfinite(det) && std::abs(det) > 1e-12, "singular affine transform");
}
std::array<float, 12> Affine::relativeTo(const std::array<double, 3> &camera) const {
    std::array<float, 12> out{};
    for (size_t r = 0; r < 3; r++) {
        for (size_t c = 0; c < 3; c++) out[r * 4 + c] = linear[r * 3 + c];
        out[r * 4 + 3] = static_cast<float>(translation[r] - camera[r]);
    }
    return out;
}
void Scene::check(uint64_t e) const {
    require(e == epoch_, "stale persistent scene epoch");
}
uint64_t Scene::epoch() const {
    std::lock_guard lock(mutex_);
    return epoch_;
}
uint64_t Scene::reset() {
    std::lock_guard lock(mutex_);
    if (epoch_ == UINT64_MAX) throw std::overflow_error("persistent scene epoch exhausted");
    ++epoch_;
    serial_ = 0;
    bytes_ = 0;
    visible_.clear();
    meshes_.clear();
    materials_.clear();
    materialRevisions_.clear();
    meshRevisions_.clear();
    previous_.reset();
    return epoch_;
}
void Scene::material(uint64_t e, Material value) {
    require(value.id && value.revision, "zero material id/revision");
    require(value.alphaMode <= 1, "only opaque and cutout materials supported");
    require(std::isfinite(value.emission) && value.emission >= 0 && value.emission <= 100, "invalid emission");
    std::lock_guard lock(mutex_);
    check(e);
    auto it = materials_.find(value.id);
    auto seen = materialRevisions_.find(value.id);
    require(seen == materialRevisions_.end() || value.revision > seen->second, "stale material revision");
    require(seen != materialRevisions_.end() || materialRevisions_.size() < 65536,
            "material ID capacity exceeded; reset epoch");
    materialRevisions_[value.id] = value.revision;
    materials_[value.id] = std::make_shared<Material>(std::move(value));
}
void Scene::mesh(uint64_t e,
                 uint64_t id,
                 uint64_t revision,
                 uint64_t materialId,
                 uint64_t materialRevision,
                 std::span<const std::byte> packet) {
    require(id && revision, "zero mesh id/revision");
    Reader r{packet};
    require(r.get<uint32_t>() == magicMesh && r.get<uint32_t>() == 1, "unsupported mesh packet");
    uint32_t vc = r.get<uint32_t>(), ic = r.get<uint32_t>();
    require(vc >= 3 && vc <= 1048576 && ic >= 3 && ic <= 3145728 && ic % 3 == 0, "invalid mesh counts");
    require(packet.size() == 16ull + 48ull * vc + 4ull * ic, "invalid mesh packet length");
    auto value = std::make_shared<Mesh>();
    value->id = id;
    value->revision = revision;
    value->vertices.resize(vc);
    value->indices.resize(ic);
    value->min.fill(std::numeric_limits<float>::infinity());
    value->max.fill(-std::numeric_limits<float>::infinity());
    for (auto &v : value->vertices) {
        for (float &f : v.position) {
            f = r.get<float>();
            require(std::isfinite(f) && std::abs(f) <= 1e6f, "invalid local position");
        }
        double normalLength = 0;
        for (float &f : v.normal) {
            f = r.get<float>();
            require(std::isfinite(f) && std::abs(f) <= 1e6f, "invalid normal");
            normalLength += double(f) * f;
        }
        require(normalLength > 1e-12, "zero vertex normal");
        for (float &f : v.uv) {
            f = r.get<float>();
            require(std::isfinite(f) && std::abs(f) <= 1e6f, "invalid UV");
        }
        for (float &f : v.color) {
            f = r.get<float>();
            require(std::isfinite(f) && f >= 0 && f <= 1, "invalid vertex color");
        }
        for (int i = 0; i < 3; i++) {
            value->min[i] = std::min(value->min[i], v.position[i]);
            value->max[i] = std::max(value->max[i], v.position[i]);
        }
    }
    for (auto &i : value->indices) {
        i = r.get<uint32_t>();
        require(i < vc, "mesh index out of range");
    }
    r.end();
    std::lock_guard lock(mutex_);
    check(e);
    auto mat = materials_.find(materialId);
    require(mat != materials_.end() && mat->second->revision == materialRevision, "unknown material revision");
    value->material = mat->second;
    auto old = meshes_.find(id);
    auto seen = meshRevisions_.find(id);
    require(seen == meshRevisions_.end() || revision > seen->second, "stale mesh revision");
    require(seen != meshRevisions_.end() || meshRevisions_.size() < 65536, "mesh ID capacity exceeded; reset epoch");
    size_t next = bytes_ - (old == meshes_.end() ? 0 : meshBytes(*old->second)) + meshBytes(*value);
    require(next <= budget, "resident mesh CPU budget exceeded");
    // Existing instances retain the immutable old revision until their next submitted visible set.
    meshRevisions_[id] = revision;
    meshes_[id] = std::move(value);
    bytes_ = next;
}
void Scene::instances(uint64_t e, std::span<const std::byte> packet) {
    Reader r{packet};
    require(r.get<uint32_t>() == magicInstances && r.get<uint32_t>() == 1, "unsupported instance packet");
    uint32_t count = r.get<uint32_t>();
    require(r.get<uint32_t>() == 0, "nonzero instance reserved field");
    require(count <= 65536 && packet.size() == 16ull + 84ull * count, "invalid instance packet length/count");
    std::lock_guard lock(mutex_);
    check(e);
    std::map<uint64_t, Instance> next;
    for (uint32_t i = 0; i < count; i++) {
        Instance v;
        v.id = r.get<uint64_t>();
        uint64_t meshId = r.get<uint64_t>(), revision = r.get<uint64_t>();
        require(v.id, "zero instance id");
        for (float &f : v.transform.linear) f = r.get<float>();
        for (double &d : v.transform.translation) d = r.get<double>();
        v.transform.validate();
        auto m = meshes_.find(meshId);
        require(m != meshes_.end() && m->second->revision == revision, "unknown mesh revision");
        v.mesh = m->second;
        require(next.emplace(v.id, std::move(v)).second, "duplicate instance id");
    }
    r.end();
    visible_.swap(next);
}
void Scene::retireMesh(uint64_t e, uint64_t id) {
    std::lock_guard lock(mutex_);
    check(e);
    auto it = meshes_.find(id);
    if (it != meshes_.end()) {
        bytes_ -= meshBytes(*it->second);
        meshes_.erase(it);
    }
    for (auto v = visible_.begin(); v != visible_.end();)
        if (v->second.mesh->id == id)
            v = visible_.erase(v);
        else
            ++v;
}
void Scene::retireMaterial(uint64_t e, uint64_t id) {
    std::lock_guard lock(mutex_);
    check(e);
    materials_.erase(id);
}
std::shared_ptr<Frame> Scene::snapshot(const std::array<double, 3> &camera) {
    for (double d : camera) require(std::isfinite(d) && std::abs(d) <= 1e12, "invalid camera origin");
    std::lock_guard lock(mutex_);
    auto frame = std::make_shared<Frame>();
    frame->epoch = epoch_;
    frame->serial = ++serial_;
    frame->camera = camera;
    std::map<uint64_t, const Draw *> prior;
    if (previous_)
        for (auto &d : previous_->draws) prior.emplace(d.current.id, &d);
    for (auto &[id, v] : visible_) {
        Draw draw{v, v.transform, camera, false};
        auto p = prior.find(id);
        if (p != prior.end() && p->second->current.mesh == v.mesh) {
            draw.history = true;
            draw.previous = p->second->current.transform;
            draw.previousCamera = previous_->camera;
        }
        frame->draws.push_back(std::move(draw));
    }
    previous_ = frame;
    return frame;
}
} // namespace persistent
