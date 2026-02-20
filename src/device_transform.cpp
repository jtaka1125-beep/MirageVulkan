#include "device_transform.hpp"

namespace mirage::gui {

void DeviceTransform::rotated_dims(int& rw, int& rh) const {
    int r = rotation % 360;
    if (r < 0) r += 360;
    if (r == 90 || r == 270) {
        rw = video_h;
        rh = video_w;
    } else {
        rw = video_w;
        rh = video_h;
    }
}

void DeviceTransform::video_to_rotated(float vx, float vy, float& rx, float& ry) const {
    int r = rotation % 360;
    if (r < 0) r += 360;

    // Define rotation as clockwise degrees to convert video -> rotated (native-oriented) coordinates.
    // Rotation is applied around the top-left origin in pixel space.
    switch (r) {
        case 0:
            rx = vx; ry = vy;
            break;
        case 90:
            // (w,h) -> (h,w)
            // x' = h-1 - y, y' = x
            rx = static_cast<float>(video_h - 1) - vy;
            ry = vx;
            break;
        case 180:
            rx = static_cast<float>(video_w - 1) - vx;
            ry = static_cast<float>(video_h - 1) - vy;
            break;
        case 270:
            // x' = y, y' = w-1 - x
            rx = vy;
            ry = static_cast<float>(video_w - 1) - vx;
            break;
        default:
            // Unsupported angle: treat as identity
            rx = vx; ry = vy;
            break;
    }
}

void DeviceTransform::rotated_to_video(float rx, float ry, float& vx, float& vy) const {
    int r = rotation % 360;
    if (r < 0) r += 360;

    // Inverse mapping of video_to_rotated
    switch (r) {
        case 0:
            vx = rx; vy = ry;
            break;
        case 90:
            // rx = h-1 - vy, ry = vx
            vx = ry;
            vy = static_cast<float>(video_h - 1) - rx;
            break;
        case 180:
            vx = static_cast<float>(video_w - 1) - rx;
            vy = static_cast<float>(video_h - 1) - ry;
            break;
        case 270:
            // rx = vy, ry = w-1 - vx
            vx = static_cast<float>(video_w - 1) - ry;
            vy = rx;
            break;
        default:
            vx = rx; vy = ry;
            break;
    }
}

void DeviceTransform::recalculate() {
    // Basic sanity
    if (native_w <= 0 || native_h <= 0 || video_w <= 0 || video_h <= 0) {
        scale_x = scale_y = 1.0f;
        offset_x = offset_y = 0.0f;
        return;
    }

    // If the video is a bottom-trimmed variant of native (e.g., nav bar removed), keep 1:1 mapping.
    // This avoids centering offsets that would break touch/AI coordinates.
    {
        int rw, rh;
        rotated_dims(rw, rh);
        // Only handle the no-rotation case here; rotated trims can be added later if needed.
        int r = rotation % 360; if (r < 0) r += 360;
        if (r == 0 && rw == native_w && rh <= native_h) {
            int diff = native_h - rh;
            if (diff > 0 && diff <= nav_bar_tolerance_px) {
                scale_x = scale_y = 1.0f;
                offset_x = 0.0f;
                offset_y = 0.0f;
                return;
            }
        }
    }

    int rw, rh;
    rotated_dims(rw, rh);
    if (rw <= 0 || rh <= 0) {
        scale_x = scale_y = 1.0f;
        offset_x = offset_y = 0.0f;
        return;
    }

    float sx = static_cast<float>(native_w) / static_cast<float>(rw);
    float sy = static_cast<float>(native_h) / static_cast<float>(rh);

    float s = crop ? std::max(sx, sy) : std::min(sx, sy);
    scale_x = s;
    scale_y = s;

    offset_x = (static_cast<float>(native_w) - static_cast<float>(rw) * s) * 0.5f;
    offset_y = (static_cast<float>(native_h) - static_cast<float>(rh) * s) * 0.5f;
}

void DeviceTransform::video_to_native(float vx, float vy, float& nx, float& ny) const {
    float rx, ry;
    video_to_rotated(vx, vy, rx, ry);
    nx = rx * scale_x + offset_x;
    ny = ry * scale_y + offset_y;
}

void DeviceTransform::native_to_video(float nx, float ny, float& vx, float& vy) const {
    // Inverse scale/offset
    float rx = (nx - offset_x) / (scale_x == 0.0f ? 1.0f : scale_x);
    float ry = (ny - offset_y) / (scale_y == 0.0f ? 1.0f : scale_y);

    // Inverse rotation
    rotated_to_video(rx, ry, vx, vy);

    // Optional clamp to valid range (avoid NaNs / negative)
    vx = std::clamp(vx, 0.0f, static_cast<float>(std::max(0, video_w - 1)));
    vy = std::clamp(vy, 0.0f, static_cast<float>(std::max(0, video_h - 1)));
}

} // namespace mirage::gui
