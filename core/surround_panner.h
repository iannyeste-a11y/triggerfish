#pragma once
#include <cmath>
#include <algorithm>
#include <utility>

namespace radium {

struct Surround51Gains {
    float left = 0.0f;
    float right = 0.0f;
    float center = 0.0f;
    float left_surround = 0.0f;
    float right_surround = 0.0f;
};

struct Surround70Gains {
    float left = 0.0f;
    float right = 0.0f;
    float center = 0.0f;
    float left_side = 0.0f;
    float right_side = 0.0f;
    float left_rear = 0.0f;
    float right_rear = 0.0f;
};

inline Surround51Gains surround_51_gains(float x, float y) {
    x = std::clamp(x, -1.0f, 1.0f);
    y = std::clamp(y, -1.0f, 1.0f);

    const float front = (y + 1.0f) * 0.5f;
    const float rear  = 1.0f - front;
    const float left  = (1.0f - x) * 0.5f;
    const float right = (1.0f + x) * 0.5f;

    const float center_amount = std::max(0.0f, 1.0f - std::abs(x));
    constexpr float kCenterScale = 0.5f;

    return {
        front * left,
        front * right,
        front * center_amount * kCenterScale,
        rear * left,
        rear * right
    };
}

inline Surround70Gains surround_70_gains(float x, float y) {
    x = std::clamp(x, -1.0f, 1.0f);
    y = std::clamp(y, -1.0f, 1.0f);

    const float front = std::clamp(y, 0.0f, 1.0f);
    const float side = 1.0f - std::abs(y);
    const float rear = std::clamp(-y, 0.0f, 1.0f);
    const float left = (1.0f - x) * 0.5f;
    const float right = (1.0f + x) * 0.5f;
    const float center_amount = std::max(0.0f, 1.0f - std::abs(x));
    constexpr float kCenterScale = 0.5f;

    return {
        front * left,
        front * right,
        front * center_amount * kCenterScale,
        side * left,
        side * right,
        rear * left,
        rear * right
    };
}

inline Surround51Gains fold_70_to_51(const Surround70Gains& gains) {
    constexpr float kSideBlend = 0.7071067811865476f;
    return {
        gains.left,
        gains.right,
        gains.center,
        gains.left_rear + gains.left_side * kSideBlend,
        gains.right_rear + gains.right_side * kSideBlend
    };
}

// Compute stereo fold-down gains from a 5.1 surround panner position.
// x: [-1, 1] left to right.  y: [-1, 1] rear to front.
// Returns {left_stereo_gain, right_stereo_gain} with power normalization.
inline std::pair<float, float> surround_folddown(float x, float y) {
    const auto gains = fold_70_to_51(surround_70_gains(x, y));

    // ITU-R BS.775 stereo fold-down
    constexpr float k = 0.7071067811865476f;  // 1/sqrt(2)
    float left_out  = gains.left + k * gains.center + k * gains.left_surround;
    float right_out = gains.right + k * gains.center + k * gains.right_surround;

    // Power normalization
    const float power = left_out * left_out + right_out * right_out;
    if (power > 1e-12f) {
        const float scale = 1.0f / std::sqrt(power);
        left_out  *= scale;
        right_out *= scale;
    }

    return {left_out, right_out};
}

}  // namespace radium
