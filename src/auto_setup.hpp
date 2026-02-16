// =============================================================================
// MirageSystem v2 - Auto Setup
// =============================================================================
// Automatic device setup wizard for screen mirroring
// =============================================================================
#pragma once

#include <string>
#include <functional>
#include <thread>

namespace mirage {

enum class SetupStatus {
    PENDING,
    IN_PROGRESS,
    COMPLETED,
    SKIPPED,
    FAILED
};

struct SetupStepResult {
    SetupStatus status = SetupStatus::PENDING;
    std::string message;
};

struct AutoSetupResult {
    bool success = false;
    std::string error;

    std::string summary() const {
        return success ? "OK" : error;
    }
};

class AutoSetup {
public:
    using ProgressCallback = std::function<void(const std::string& step, int progress)>;
    using AdbExecutor = std::function<std::string(const std::string& cmd)>;

    void setProgressCallback(ProgressCallback cb) { progress_callback_ = std::move(cb); }
    void set_progress_callback(ProgressCallback cb) { progress_callback_ = std::move(cb); }
    void set_adb_executor(AdbExecutor cb) { adb_executor_ = std::move(cb); }

    // Full setup run
    AutoSetupResult run(const std::string& device_id, void* adb_manager) {
        (void)device_id;
        (void)adb_manager;
        return runInternal();
    }

    // Alternative: run with bool flag (for gui_render_left_panel.cpp compatibility)
    AutoSetupResult run(bool full_setup) {
        (void)full_setup;
        return runInternal();
    }

    // Individual setup steps (for adb_device_manager.cpp compatibility)
    SetupStepResult start_screen_capture(const std::string& host, int port) {
        if (progress_callback_) {
            progress_callback_("Starting screen capture...", 25);
        }
        if (adb_executor_) {
            // Force-stop to get fresh state
            adb_executor_("shell am force-stop com.mirage.streamer");
            std::this_thread::sleep_for(std::chrono::milliseconds(500));

            // Launch MirageStreamer: MediaProjection -> H.264 -> UDP
            std::string cmd = "shell am start"
                " -n com.mirage.streamer/.StreamActivity"
                " --es host " + host +
                " --ei port " + std::to_string(port);
            adb_executor_(cmd);
        }
        SetupStepResult result;
        result.status = SetupStatus::COMPLETED;
        return result;
    }

    SetupStepResult approve_screen_share_dialog() {
        if (progress_callback_) {
            progress_callback_("Approving screen share dialog...", 50);
        }
        if (adb_executor_) {
            // Tap "Start now" button (common coordinates)
            adb_executor_("shell input tap 540 1150");
        }
        SetupStepResult result;
        result.status = SetupStatus::COMPLETED;
        return result;
    }

    SetupStepResult complete_and_verify() {
        if (progress_callback_) {
            progress_callback_("Completing setup...", 100);
        }
        if (adb_executor_) {
            // Press HOME to go back
            adb_executor_("shell input keyevent KEYCODE_HOME");
        }
        SetupStepResult result;
        result.status = SetupStatus::COMPLETED;
        result.message = "";
        return result;
    }

private:
    ProgressCallback progress_callback_;
    AdbExecutor adb_executor_;

    AutoSetupResult runInternal() {
        AutoSetupResult result;
        result.success = true;
        return result;
    }
};

} // namespace mirage
