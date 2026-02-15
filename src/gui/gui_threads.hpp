// =============================================================================
// MirageSystem v2 GUI - Background Threads Header
// =============================================================================
#pragma once

namespace mirage::gui::threads {

// ADB device detection thread (runs at startup)
void adbDetectionThread();

// Device update thread (runs continuously for video/stats)
void deviceUpdateThread();

// WiFi ADB watchdog (detached, runs continuously)
// Re-enables WiFi ADB on devices after reboot (critical for remote operation)
void wifiAdbWatchdogThread();

} // namespace mirage::gui::threads
