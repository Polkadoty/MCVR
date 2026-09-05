#pragma once
#include <cstdint>
#include "common/shared.hpp"
#include <sl_consts.h>
#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>
#include <cmath>

namespace frame_gen {
// GLM multiplies column vectors; Streamline's matrix helpers multiply row
// vectors. A GLM column therefore becomes one SL row (transpose the operator).
inline sl::float4x4 toSlRowVectorMatrix(const glm::mat4 &matrix) {
    sl::float4x4 result{};
    for (int row = 0; row < 4; ++row)
        result[row] = sl::float4(matrix[row][0], matrix[row][1], matrix[row][2], matrix[row][3]);
    return result;
}
inline float normalizedDepth(float positiveViewDepth, const glm::mat4 &projection, float farDepth) {
    if (!std::isfinite(positiveViewDepth) || positiveViewDepth <= 0 || positiveViewDepth >= INF_DISTANCE)
        return farDepth;
    float z = -positiveViewDepth;
    float w = projection[2][3] * z + projection[3][3];
    if (std::abs(w) < 1e-20f) return farDepth;
    return std::clamp((projection[2][2] * z + projection[3][2]) / w, 0.0f, 1.0f);
}
inline bool finite(const glm::mat4 &matrix) {
    for (int c = 0; c < 4; ++c) for (int r = 0; r < 4; ++r)
        if (!std::isfinite(matrix[c][r])) return false;
    return true;
}
class CameraHistory {
    bool valid_{};
    uint32_t frame_{}, width_{}, height_{};
    vk::Data::WorldUBO previous_{};
public:
    void reset() { valid_ = false; }
    bool fill(const vk::Data::WorldUBO &world, uint32_t width, uint32_t height,
              uint32_t frame, bool forcedReset, sl::Constants &out) {
        const auto &view = world.cameraEffectedViewMat;
        const auto &projection = world.cameraProjMat;
        const auto inverseView = glm::inverse(view);
        const auto inverseProjection = glm::inverse(projection);
        if (!width || !height || !finite(view) || !finite(projection)
            || !finite(inverseView) || !finite(inverseProjection)) return false;
        auto planeDistance = [&](float depth) {
            const auto position = inverseProjection * glm::vec4(0, 0, depth, 1);
            return std::abs(position.w) < 1e-12f ? 1e6f : std::abs(position.z / position.w);
        };
        float depthZeroDistance = planeDistance(0), depthOneDistance = planeDistance(1);
        float nearPlane = std::min(depthZeroDistance, depthOneDistance);
        float farPlane = std::max(depthZeroDistance, depthOneDistance);
        if (!std::isfinite(nearPlane) || !std::isfinite(farPlane) || nearPlane <= 0 || farPlane <= nearPlane
            || std::abs(projection[0][0]) < 1e-8f || std::abs(projection[1][1]) < 1e-8f) return false;
        const bool discontinuity = valid_ && glm::length(glm::dvec3(world.cameraPos) - glm::dvec3(previous_.cameraPos)) > 32.0;
        const bool resetFrame = forcedReset || discontinuity || !valid_ || frame != frame_ + 1 || width != width_ || height != height_;
        const auto &previous = resetFrame ? world : previous_;
        // Views are camera-relative. Subtract world origins in double precision
        // before constructing the current-relative -> previous-relative transform.
        glm::vec3 delta = glm::vec3(glm::dvec3(world.cameraPos) - glm::dvec3(previous.cameraPos));
        glm::mat4 currentToPrevious = previous.cameraProjMat * previous.cameraEffectedViewMat
            * glm::translate(glm::mat4(1), delta) * inverseView * inverseProjection;
        glm::mat4 previousToCurrent = glm::inverse(currentToPrevious);
        if (!finite(currentToPrevious) || !finite(previousToCurrent)) return false;
        out = sl::Constants{};
        out.cameraViewToClip = toSlRowVectorMatrix(projection);
        out.clipToCameraView = toSlRowVectorMatrix(inverseProjection);
        out.clipToLensClip = toSlRowVectorMatrix(glm::mat4(1));
        out.clipToPrevClip = toSlRowVectorMatrix(currentToPrevious);
        out.prevClipToClip = toSlRowVectorMatrix(previousToCurrent);
        out.jitterOffset = sl::float2(world.cameraJitter.x, world.cameraJitter.y);
        out.mvecScale = sl::float2(1.0f / width, 1.0f / height);
        out.cameraPinholeOffset = sl::float2(0, 0);
        auto copyVector = [](glm::vec3 v) { return sl::float3(v.x, v.y, v.z); };
        out.cameraPos = copyVector(glm::vec3(glm::dvec3(world.cameraPos) + glm::dvec3(inverseView[3])));
        out.cameraRight = copyVector(glm::normalize(glm::vec3(inverseView[0])));
        out.cameraUp = copyVector(glm::normalize(glm::vec3(inverseView[1])));
        out.cameraFwd = copyVector(glm::normalize(-glm::vec3(inverseView[2])));
        out.cameraNear = nearPlane;
        out.cameraFar = farPlane;
        out.cameraFOV = 2.0f * std::atan(1.0f / std::abs(projection[1][1]));
        out.cameraAspectRatio = std::abs(projection[1][1] / projection[0][0]);
        out.depthInverted = depthZeroDistance > depthOneDistance ? sl::Boolean::eTrue : sl::Boolean::eFalse;
        out.cameraMotionIncluded = sl::Boolean::eTrue;
        out.motionVectors3D = sl::Boolean::eFalse;
        out.motionVectorsDilated = sl::Boolean::eFalse;
        out.motionVectorsJittered = sl::Boolean::eFalse;
        out.reset = resetFrame ? sl::Boolean::eTrue : sl::Boolean::eFalse;
        previous_ = world; frame_ = frame; width_ = width; height_ = height; valid_ = true;
        return true;
    }
};
} // namespace frame_gen
