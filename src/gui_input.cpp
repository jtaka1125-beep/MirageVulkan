// =============================================================================
// MirageSystem v2 - GUI Implementation Part 3
// =============================================================================
// Input handling: Mouse clicks, double-clicks, drag/swipe, learning mode
// =============================================================================
#include "gui/gui_state.hpp"
#include "gui_application.hpp"
#include "hybrid_command_sender.hpp"

#include <imgui.h>
#include <cmath>
#include <algorithm>
#include "mirage_log.hpp"

namespace mirage::gui {

using namespace mirage::gui::state;

// =============================================================================
// Coordinate Conversion
// =============================================================================

std::pair<int, int> GuiApplication::screenToDeviceCoords(
        const DeviceInfo& device,
        float view_x, float view_y,
        float view_w, float view_h,
        int screen_x, int screen_y) const {
    
    // Check for valid dimensions (prevent division by zero)
    if (device.texture_width <= 0 || device.texture_height <= 0) {
        return {-1, -1};
    }
    
    if (view_w <= 0.0f || view_h <= 0.0f) {
        return {-1, -1};
    }
    
    // Check if within view bounds
    if (screen_x < view_x || screen_x >= view_x + view_w ||
        screen_y < view_y || screen_y >= view_y + view_h) {
        return {-1, -1};
    }
    
    // Convert to device coordinates
    float rel_x = (screen_x - view_x) / view_w;
    float rel_y = (screen_y - view_y) / view_h;
    
    int dev_x = static_cast<int>(rel_x * device.texture_width);
    int dev_y = static_cast<int>(rel_y * device.texture_height);
    
    // Clamp
    dev_x = std::max(0, std::min(dev_x, device.texture_width - 1));
    dev_y = std::max(0, std::min(dev_y, device.texture_height - 1));
    
    return {dev_x, dev_y};
}

// =============================================================================
// Mouse Input Handlers
// =============================================================================

void GuiApplication::onMouseMove(int x, int y) {
    mouse_x_ = x;
    mouse_y_ = y;
    
    // Update hovered panel
    auto layout = calculateLayout();
    
    if (x < layout.left_w) {
        hovered_panel_ = HoveredPanel::Left;
    } else if (x < layout.left_w + layout.center_w) {
        hovered_panel_ = HoveredPanel::Center;
    } else {
        hovered_panel_ = HoveredPanel::Right;
    }
    
    // Handle dragging (for swipe)
    if (is_dragging_ && mouse_down_[0]) {
        // Update drag visualization if needed
    }
}

void GuiApplication::onMouseDown(int button, int x, int y) {
    if (button >= 0 && button < 3) {
        mouse_down_[button] = true;
    }
    
    if (button == 0) {  // Left button
        drag_start_x_ = x;
        drag_start_y_ = y;
        is_dragging_ = false;  // Will be set true if moved enough
    }
}

void GuiApplication::onMouseUp(int button, int x, int y) {
    if (button >= 0 && button < 3) {
        mouse_down_[button] = false;
    }
    
    if (button == 0) {  // Left button
        auto layout = calculateLayout();
        
        // Check for drag (swipe)
        int dx = x - drag_start_x_;
        int dy = y - drag_start_y_;
        float drag_dist = std::sqrt(static_cast<float>(dx * dx + dy * dy));

        if (drag_dist > layout_constants::MIN_SWIPE_DISTANCE) {
            // This was a swipe
            processSwipe(drag_start_x_, drag_start_y_, x, y);
        } else {
            // This was a tap/click

            if (hovered_panel_ == HoveredPanel::Center) {
                // Both x and y should be relative to center panel
                processMainViewClick(
                    x - static_cast<int>(layout.center_x),
                    y,  // y is window-relative (header height handled in processMainViewClick)
                    false
                );
            } else if (hovered_panel_ == HoveredPanel::Right) {
                processSubViewClick(
                    x - static_cast<int>(layout.right_x),
                    y,
                    false
                );
            }
        }
        
        is_dragging_ = false;
    }
}

void GuiApplication::onMouseDoubleClick(int button, int x, int y) {
    if (button != 0) return;  // Only handle left double-click
    
    auto layout = calculateLayout();
    
    if (hovered_panel_ == HoveredPanel::Right) {
        processSubViewClick(
            x - static_cast<int>(layout.right_x),
            y,
            true  // double click
        );
    }
}

// =============================================================================
// Click Processing
// =============================================================================

void GuiApplication::processMainViewClick(int local_x, int local_y, bool /*is_double*/) {
    // Variables to hold data for callbacks (called outside mutex)
    std::string device_id;
    int dev_x = -1, dev_y = -1;
    bool should_tap_callback = false;
    bool should_learning_callback = false;
    LearningClickData learning_data;
    
    {
        std::lock_guard<std::mutex> lock(devices_mutex_);

        if (main_device_id_.empty()) {
            return;
        }

        auto it = devices_.find(main_device_id_);
        if (it == devices_.end()) {
            return;
        }
        
        DeviceInfo& device = it->second;
        device_id = device.id;
        
        // Use stored view rectangle from rendering (most accurate)
        // This is set by renderDeviceView() during the render loop
        float view_x, view_y, view_w, view_h;
        auto layout = calculateLayout();
        
        // Thread-safe read of view rect
        ViewRect cached_rect;
        {
            std::lock_guard<std::mutex> rect_lock(view_rect_mutex_);
            cached_rect = main_view_rect_;
        }

        if (cached_rect.valid) {
            // Use the exact coordinates from rendering
            // main_view_rect_ is in window coordinates, so convert local_x to window coords
            int window_x = local_x + static_cast<int>(layout.center_x);
            int window_y = local_y;  // y is already window-relative

            view_x = cached_rect.x;
            view_y = cached_rect.y;
            view_w = cached_rect.w;
            view_h = cached_rect.h;

            // Use window coordinates for comparison
            auto [dx, dy] = screenToDeviceCoords(
                device, view_x, view_y, view_w, view_h,
                static_cast<float>(window_x), static_cast<float>(window_y)
            );
            dev_x = dx;
            dev_y = dy;
            
            if (dev_x < 0 || dev_y < 0) {
                return;
            }
        } else {
            // Fallback: Calculate view rectangle manually (less accurate, pre-render or no texture)
            
            const float header_height = layout_constants::PANEL_HEADER_HEIGHT;
            
            view_w = layout.center_w;
            view_h = layout.height - header_height;
            view_x = 0;  // Relative to center panel
            view_y = header_height;  // Below header
            
            if (device.texture_width > 0 && device.texture_height > 0 && view_h > 0) {
                float aspect = static_cast<float>(device.texture_width) / device.texture_height;
                float container_aspect = view_w / view_h;
                
                if (aspect > container_aspect) {
                    float new_h = (aspect > 0) ? (view_w / aspect) : view_h;
                    view_y += (view_h - new_h) / 2;
                    view_h = new_h;
                } else {
                    float new_w = view_h * aspect;
                    view_x += (view_w - new_w) / 2;
                    view_w = new_w;
                }
            }
            
            // Convert to device coordinates (using Android's native resolution)
            auto [dx, dy] = screenToDeviceCoords(
                device, view_x, view_y, view_w, view_h,
                static_cast<float>(local_x), static_cast<float>(local_y)
            );
            dev_x = dx;
            dev_y = dy;

            if (dev_x < 0 || dev_y < 0) return;  // Click outside view
        }
        
        // Learning mode: collect data before executing (common for both paths)
        // Use separate learning_mutex_ for thread safety
        {
            std::lock_guard<std::mutex> learning_lock(learning_mutex_);
            if (learning_session_.active) {
                learning_data = collectLearningData(device, dev_x, dev_y);
                learning_session_.collected_clicks.push_back(learning_data);
                should_learning_callback = true;

                logDebug("Learning: collected click at (" + std::to_string(dev_x) + ", " +
                         std::to_string(dev_y) + ") with " +
                         std::to_string(learning_data.relative_positions.size()) + " nearby elements");
            }
        }
        
        // Prepare for tap callback
        should_tap_callback = (tap_callback_ != nullptr);

        logDebug("Tap: " + device_id + " @ (" + std::to_string(dev_x) + ", " +
                 std::to_string(dev_y) + ")");
    }
    
    // Call callbacks OUTSIDE mutex to avoid deadlock
    if (should_learning_callback && learning_data_callback_) {
        learning_data_callback_(learning_data);
    }
    
    if (should_tap_callback && tap_callback_) {
        tap_callback_(device_id, dev_x, dev_y);
    }
}

void GuiApplication::processSubViewClick(int panel_x, int panel_y, bool is_double) {
    std::string selected_device_id;
    bool should_callback = false;
    
    {
        std::lock_guard<std::mutex> lock(devices_mutex_);
        
        // Get sub devices
        std::vector<std::string> sub_devices;
        for (const auto& id : device_order_) {
            if (id != main_device_id_) {
                sub_devices.push_back(id);
            }
        }
        
        if (sub_devices.empty()) return;
        
        // Calculate grid (use unified header height constant)
        auto layout = calculateLayout();
        const float header_height = layout_constants::PANEL_HEADER_HEIGHT;
        float avail_h = layout.height - header_height;
        auto grid = calculateSubGrid(static_cast<int>(devices_.size()), layout.right_w, avail_h);
        
        float padding = static_cast<float>(config_.sub_grid_padding);
        
        // Protect against division by zero or negative values
        float cell_w_total = grid.cell_w + padding;
        float cell_h_total = grid.cell_h + padding;
        if (cell_w_total <= 0 || cell_h_total <= 0) return;
        
        // Find which cell was clicked
        int col = static_cast<int>((panel_x - padding) / cell_w_total);
        int row = static_cast<int>((panel_y - header_height - padding) / cell_h_total);
        
        if (col < 0 || col >= grid.cols || row < 0 || row >= grid.rows) return;
        
        int idx = row * grid.cols + col;
        if (idx >= static_cast<int>(sub_devices.size())) return;
        
        const std::string& device_id = sub_devices[idx];
        
        if (is_double) {
            // Swap with main
            main_device_id_ = device_id;
            {
                std::lock_guard<std::mutex> rect_lock(view_rect_mutex_);
                main_view_rect_.valid = false;  // Reset view rect when main device changes
            }
            selected_device_id = device_id;
            should_callback = true;
            logInfo("Swapped main device: " + device_id);
        } else {
            // Single click on sub device - could show details or highlight
            hovered_device_id_ = device_id;
        }
    }
    
    // Call callback OUTSIDE mutex to avoid deadlock
    if (should_callback && device_select_callback_) {
        device_select_callback_(selected_device_id);
    }
}

// =============================================================================
// Swipe Processing
// =============================================================================

void GuiApplication::processSwipe(int x1, int y1, int x2, int y2) {
    auto layout = calculateLayout();
    
    // Only process swipes in center panel
    if (x1 < layout.center_x || x1 >= layout.center_x + layout.center_w) return;
    
    // Variables for callback (called outside mutex)
    std::string device_id;
    int dev_x1 = -1, dev_y1 = -1, dev_x2 = -1, dev_y2 = -1;
    int duration_ms = 0;
    bool should_callback = false;
    
    {
        std::lock_guard<std::mutex> lock(devices_mutex_);
        
        if (main_device_id_.empty()) return;
        
        auto it = devices_.find(main_device_id_);
        if (it == devices_.end()) return;
        
        DeviceInfo& device = it->second;
        device_id = device.id;
        
        // Use stored view rectangle from rendering when available
        float view_x, view_y, view_w, view_h;

        // Thread-safe read of view rect
        ViewRect cached_rect;
        {
            std::lock_guard<std::mutex> rect_lock(view_rect_mutex_);
            cached_rect = main_view_rect_;
        }

        if (cached_rect.valid) {
            // Use the exact coordinates from rendering (window coordinates)
            view_x = cached_rect.x;
            view_y = cached_rect.y;
            view_w = cached_rect.w;
            view_h = cached_rect.h;
        } else {
            // Fallback: Calculate manually
            const float header_height = layout_constants::PANEL_HEADER_HEIGHT;
            view_w = layout.center_w;
            view_h = layout.height - header_height;
            view_x = layout.center_x;
            view_y = header_height;
            
            if (device.texture_width > 0 && device.texture_height > 0 && view_h > 0) {
                float aspect = static_cast<float>(device.texture_width) / device.texture_height;
                float container_aspect = view_w / view_h;
                
                if (aspect > container_aspect) {
                    float new_h = (aspect > 0) ? (view_w / aspect) : view_h;
                    view_y += (view_h - new_h) / 2;
                    view_h = new_h;
                } else {
                    float new_w = view_h * aspect;
                    view_x += (view_w - new_w) / 2;
                    view_w = new_w;
                }
            }
        }
        
        // Convert both points to device coordinates
        auto [dx1, dy1] = screenToDeviceCoords(
            device, view_x, view_y, view_w, view_h, 
            static_cast<float>(x1), static_cast<float>(y1)
        );
        auto [dx2, dy2] = screenToDeviceCoords(
            device, view_x, view_y, view_w, view_h, 
            static_cast<float>(x2), static_cast<float>(y2)
        );
        dev_x1 = dx1; dev_y1 = dy1;
        dev_x2 = dx2; dev_y2 = dy2;
        
        if (dev_x1 < 0 || dev_y1 < 0 || dev_x2 < 0 || dev_y2 < 0) return;
        
        // Calculate swipe duration based on distance
        float dist = std::sqrt(static_cast<float>(
            (dev_x2 - dev_x1) * (dev_x2 - dev_x1) + 
            (dev_y2 - dev_y1) * (dev_y2 - dev_y1)
        ));
        duration_ms = std::max(layout_constants::MIN_SWIPE_DURATION_MS,
                               std::min(layout_constants::MAX_SWIPE_DURATION_MS,
                                        static_cast<int>(dist * layout_constants::SWIPE_DURATION_FACTOR)));
        
        should_callback = (swipe_callback_ != nullptr);

        logDebug("Swipe: " + device_id + " (" + std::to_string(dev_x1) + "," +
                 std::to_string(dev_y1) + ") -> (" + std::to_string(dev_x2) + "," +
                 std::to_string(dev_y2) + ") " + std::to_string(duration_ms) + "ms");
    }

    // Call callback OUTSIDE mutex to avoid deadlock
    if (should_callback && swipe_callback_) {
        swipe_callback_(device_id, dev_x1, dev_y1, dev_x2, dev_y2, duration_ms);
    }
}

// =============================================================================
// Keyboard Input
// =============================================================================

void GuiApplication::onKeyDown(int vkey) {
    // Handle global shortcuts
    switch (vkey) {
        case VK_F1:
            // Toggle help
            break;
            
        case VK_F2:
            // F2: Send tap to all devices (for testing)
            if (g_hybrid_cmd) {
                int count = g_hybrid_cmd->send_tap_all(
                    static_cast<int>(layout_constants::TEST_TAP_X),
                    static_cast<int>(layout_constants::TEST_TAP_Y),
                    layout_constants::DEFAULT_SCREEN_W,
                    layout_constants::DEFAULT_SCREEN_H);
                logInfo(u8"F2: 全デバイスにタップ送信 (" + std::to_string(count) + u8"台)");
            }
            break;

        case VK_F3:
            // F3: Send home key to all devices
            if (g_hybrid_cmd) {
                int count = g_hybrid_cmd->send_key_all(3);  // KEYCODE_HOME
                logInfo(u8"F3: 全デバイスにホームキー送信 (" + std::to_string(count) + u8"台)");
            }
            break;

        case VK_F5:
            // Refresh
            logInfo("Refresh (F5)");
            break;
            
        case VK_ESCAPE:
            if (learning_session_.active) {
                stopLearningSession();
            }
            break;
            
        case 'L':
            if (GetKeyState(VK_CONTROL) & 0x8000) {
                // Ctrl+L: Toggle learning mode
                if (learning_session_.active) {
                    stopLearningSession();
                } else {
                    startLearningSession("Session_" + std::to_string(getCurrentTimeMs()));
                }
            }
            break;
            
        case '1': case '2': case '3': case '4': case '5':
        case '6': case '7': case '8': case '9': case '0': {
            // Number keys: Quick switch to device
            int idx = (vkey == '0') ? 9 : (vkey - '1');
            std::string target_id;
            {
                std::lock_guard<std::mutex> lock(devices_mutex_);
                if (idx < static_cast<int>(device_order_.size())) {
                    target_id = device_order_[idx];
                }
            }
            // Call setMainDevice OUTSIDE mutex to avoid deadlock
            if (!target_id.empty()) {
                setMainDevice(target_id);
            }
            break;
        }

        case VK_TAB: {
            // Tab: Cycle through devices
            std::string target_id;
            {
                std::lock_guard<std::mutex> lock(devices_mutex_);
                if (!device_order_.empty()) {
                    auto it = std::find(device_order_.begin(), device_order_.end(), main_device_id_);
                    if (it != device_order_.end()) {
                        ++it;
                        if (it == device_order_.end()) {
                            it = device_order_.begin();
                        }
                        target_id = *it;
                    }
                }
            }
            // Call setMainDevice OUTSIDE mutex to avoid deadlock
            if (!target_id.empty()) {
                setMainDevice(target_id);
            }
            break;
        }
    }
}

void GuiApplication::onKeyUp(int /*vkey*/) {
    // Currently no key-up handling needed
}

} // namespace mirage::gui
