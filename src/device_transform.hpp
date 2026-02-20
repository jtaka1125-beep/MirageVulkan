#pragma once

#include <cmath>
#include <algorithm>

namespace mirage::gui {

// DeviceTransform maps coordinates between the decoded video frame and the device's native coordinate system.
//
// - native_*: immutable logical/native resolution for automation (from devices.json)
// - video_*: current decoded video frame size (may differ due to encoder limits)
// - rotation: degrees (0/90/180/270), applying video -> native
// - scale/offset: applied after rotation (video->native) to account for letterbox/crop
//
// NOTE: This transform is intended for automation/interaction (AI/macro/touch mapping),
//       not for Vulkan UV transforms (though it can be reused).
struct DeviceTransform {
    // Native device resolution (immutable, from devices.json)
    int native_w = 0;
    int native_h = 0;

    // Actual video stream resolution (decoder output)
    int video_w = 0;
    int video_h = 0;

    // Rotation: 0, 90, 180, 270 (video->native), clockwise degrees
    int rotation = 0;

    // Scale + offset (video->native, post-rotation)
    float scale_x = 1.0f;
    float scale_y = 1.0f;
    float offset_x = 0.0f;
    float offset_y = 0.0f;

    // If true, use crop/cover policy; otherwise letterbox/contain.
    bool crop = false;

    // Nav-bar trim tolerance (px) for recognizing bottom-trimmed frames.
    int nav_bar_tolerance_px = 200;

    // Recalculate scale/offset from current sizes + rotation.
    void recalculate();

    bool is_identity() const {
        return rotation % 360 == 0 && std::abs(scale_x - 1.0f) < 1e-6f && std::abs(scale_y - 1.0f) < 1e-6f &&
               std::abs(offset_x) < 1e-6f && std::abs(offset_y) < 1e-6f &&
               native_w == video_w && native_h == video_h;
    }

    // Convert video pixel coordinate -> native coordinate
    void video_to_native(float vx, float vy, float& nx, float& ny) const;

    // Convert native coordinate -> video pixel coordinate
    void native_to_video(float nx, float ny, float& vx, float& vy) const;

private:
    void video_to_rotated(float vx, float vy, float& rx, float& ry) const;
    void rotated_to_video(float rx, float ry, float& vx, float& vy) const;
    void rotated_dims(int& rw, int& rh) const;
};

} // namespace mirage::gui
