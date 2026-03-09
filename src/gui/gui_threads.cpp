// =============================================================================
// MirageSystem v2 GUI - Background Threads
// =============================================================================
#include "mirage_log.hpp"
#include "event_bus.hpp"
#include "frame_dispatcher.hpp"
#include "gui_threads.hpp"
#include "gui_state.hpp"
#include "config_loader.hpp"
#include "mirage_config.hpp"

#include <chrono>
#include <thread>
#include <cstdio>
#include <unordered_map>

#ifdef _WIN32
#include <windows.h>
#endif

// Forward declarations from gui_init.cpp (must be outside namespace)
namespace gui {
    class MultiDeviceReceiver;
}
extern std::unique_ptr<::gui::MultiDeviceReceiver> g_multi_receiver;
namespace mirage::gui::init { bool initializeMultiReceiver(); }

namespace mirage::gui::threads {

namespace {
// Execute command without showing console window (Windows)
int execHidden(const std::string& cmd) {
#ifdef _WIN32
    STARTUPINFOA si = {};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;

    PROCESS_INFORMATION pi = {};
    std::string cmd_copy = cmd;

    if (!CreateProcessA(nullptr, cmd_copy.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        return -1;
    }

    WaitForSingleObject(pi.hProcess, 30000);
    DWORD exit_code = 0;
    GetExitCodeProcess(pi.hProcess, &exit_code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return static_cast<int>(exit_code);
#else
    return std::system(cmd.c_str());
#endif
}
} // namespace

using namespace mirage::gui::state;


// =============================================================================
// Slot Stats Structure
// =============================================================================
struct SlotInfo {
    int slot;
    std::string serial;
    int tx_ok;
    int tx_err;
    int ack_to;
    int disc;
};

// Device index mapping for port allocation
// Port = 50000 + (device_index * 100) + channel_index
static std::unordered_map<std::string, int> g_device_index_map;
static std::unordered_map<std::string, int> g_device_channel_count;
static int g_next_device_index = 0;

static uint16_t calculatePort(const std::string& serial, int channel) {
    // Get or assign device index based on serial
    auto it = g_device_index_map.find(serial);
    int device_index;
    if (it == g_device_index_map.end()) {
        device_index = g_next_device_index++;
        g_device_index_map[serial] = device_index;
        g_device_channel_count[serial] = 0;
    } else {
        device_index = it->second;
    }
    // Port = base + device*100 + channel
    return static_cast<uint16_t>(50000 + device_index * 100 + channel);
}

// Fetch slot stats from IPC (internal helper)
static std::vector<SlotInfo> fetchSlotStats() {
    std::vector<SlotInfo> result;
    
    if (!g_ipc) return result;
    
    // Request stats from miraged
    auto ipc_resp = g_ipc->request_once(R"({"type":"stats"})");
    std::string response = ipc_resp ? ipc_resp->raw_line : "";
    if (response.empty()) return result;
    
    // Parse simple JSON response
    size_t slots_pos = response.find("\"slots\"");
    if (slots_pos == std::string::npos) return result;
    
    size_t array_start = response.find('[', slots_pos);
    if (array_start == std::string::npos) return result;
    
    size_t pos = array_start;
    while (true) {
        size_t obj_start = response.find('{', pos);
        if (obj_start == std::string::npos) break;
        
        size_t obj_end = response.find('}', obj_start);
        if (obj_end == std::string::npos) break;
        
        std::string obj = response.substr(obj_start, obj_end - obj_start + 1);
        
        SlotInfo info = {0, "", 0, 0, 0, 0};
        
        size_t slot_pos = obj.find("\"slot\":");
        if (slot_pos != std::string::npos) {
            try { info.slot = std::stoi(obj.substr(slot_pos + 7)); }
            catch (const std::exception&) { info.slot = 0; }
        }

        size_t serial_pos = obj.find("\"serial\":\"");
        if (serial_pos != std::string::npos) {
            size_t serial_end = obj.find('"', serial_pos + 10);
            if (serial_end != std::string::npos) {
                info.serial = obj.substr(serial_pos + 10, serial_end - serial_pos - 10);
            }
        }

        size_t tx_ok_pos = obj.find("\"tx_ok\":");
        if (tx_ok_pos != std::string::npos) {
            try { info.tx_ok = std::stoi(obj.substr(tx_ok_pos + 8)); }
            catch (const std::exception&) { info.tx_ok = 0; }
        }

        size_t tx_err_pos = obj.find("\"tx_err\":");
        if (tx_err_pos != std::string::npos) {
            try { info.tx_err = std::stoi(obj.substr(tx_err_pos + 9)); }
            catch (const std::exception&) { info.tx_err = 0; }
        }
        
        result.push_back(info);
        pos = obj_end + 1;
    }
    
    return result;
}

// =============================================================================
// ADB Detection Thread
// =============================================================================

void adbDetectionThread() {
  try {
    MLOG_INFO("adb", "鬩幢ｽ｢隴擾ｽｴ郢晢ｽｻ驛｢譎｢・ｽ・ｰ鬩幢ｽ｢繝ｻ・ｧ郢晢ｽｻ繝ｻ・､鬩幢ｽ｢繝ｻ・ｧ郢晢ｽｻ繝ｻ・ｹ鬮ｫ・ｶ・つ髫ｲ蟷｢・ｽ・ｷ驛｢譎｢・ｽ・ｻ鬯ｯ・ｮ繝ｻ・｢髯ｷ・ｿ繝ｻ・･郢晢ｽｻ繝ｻ・ｧ驛｢譎｢・ｽ・ｻ..");
    g_adb_manager = std::make_unique<::gui::AdbDeviceManager>();
    // config.json鬩搾ｽｵ繝ｻ・ｺ郢晢ｽｻ繝ｻ・ｮadb.path鬩幢ｽ｢繝ｻ・ｧ髯懶ｽ｣繝ｻ・､髯晢ｽｲ繝ｻ・ｩ鬮ｫ・ｰ隴会ｽｦ繝ｻ・ｽ繝ｻ・･鬯ｮ・ｫ繝ｻ・ｱ郢晢ｽｻ繝ｻ・ｭ鬩幢ｽ｢繝ｻ・ｧ鬮ｦ・ｮ陷ｷ・ｶ・つ騾橸ｽｴetAdbPath (PATH鬮｣蛹・ｽｽ・ｳ髯懶ｽ｣繝ｻ・ｺ郢晢ｽｻ繝ｻ・ｾ髫ｴ蜿門ｾ励・・ｽ繝ｻ・ｭ驛｢譎｢・ｽ・ｻ
    {
        std::string adb_path;
        // 鬩搾ｽｵ繝ｻ・ｺ郢晢ｽｻ繝ｻ・ｾ鬩搾ｽｵ繝ｻ・ｺ髯ｷ・ｩ繝ｻ・ｺIRAGE_ADB_PATH鬮ｴ謇假ｽｽ・ｺ郢晢ｽｻ繝ｻ・ｰ鬮ｯ貅倥・郢晢ｽｻ郢晢ｽｻ繝ｻ・､鬨ｾ蛹・ｽｽ・ｻ髴取ｺｷ・､繧托ｽｽ・ｹ繝ｻ・ｧ髯懶ｽ｣繝ｻ・､郢晢ｽｻ繝ｻ・｢郢晢ｽｻ繝ｻ・ｺ鬯ｮ・ｫ繝ｻ・ｱ驛｢譎｢・ｽ・ｻ
        if (const char* env = std::getenv("MIRAGE_ADB_PATH")) {
            adb_path = env;
        } else {
            // config.json鬩搾ｽｵ繝ｻ・ｺ髣包ｽｵ隴趣ｽ｢繝ｻ・ｽ鬯｢諷ｧb.path鬩幢ｽ｢繝ｻ・ｧ髯橸ｽｳ陞滂ｽｲ繝ｻ・ｽ繝ｻ・ｪ郢晢ｽｻ繝ｻ・ｭ鬩幢ｽ｢繝ｻ・ｧ繝ｻ縺､ﾂ
            std::string exe_dir = mirage::config::getExeDirectory();
            std::string cfg_path = exe_dir + "\\config.json";
            std::ifstream cfg_file(cfg_path);
            if (cfg_file.is_open()) {
                std::string content((std::istreambuf_iterator<char>(cfg_file)),
                                     std::istreambuf_iterator<char>());
                // "path": "C:/..." 鬩幢ｽ｢繝ｻ・ｧ髯ｷ・ｻ陜捺ｻゑｽｽ・ｭ隶鯉ｽ｢隲､蠑ｱ繝ｻ繝ｻ・ｺ (adb 鬩幢ｽ｢繝ｻ・ｧ郢晢ｽｻ繝ｻ・ｻ鬩幢ｽ｢繝ｻ・ｧ郢晢ｽｻ繝ｻ・ｯ鬩幢ｽ｢繝ｻ・ｧ郢晢ｽｻ繝ｻ・ｷ鬩幢ｽ｢隴趣ｽ｢繝ｻ・ｽ繝ｻ・ｧ鬩幢ｽ｢隴趣ｽ｢繝ｻ・ｽ繝ｻ・ｳ鬮ｯ・ｷ・つ驛｢譎｢・ｽ・ｻ
                auto adb_pos = content.find("\"adb\"");
                if (adb_pos != std::string::npos) {
                    auto path_pos = content.find("\"path\"", adb_pos);
                    if (path_pos != std::string::npos) {
                        auto colon = content.find(':', path_pos);
                        auto quote1 = content.find('"', colon);
                        auto quote2 = content.find('"', quote1 + 1);
                        if (quote1 != std::string::npos && quote2 != std::string::npos)
                            adb_path = content.substr(quote1 + 1, quote2 - quote1 - 1);
                    }
                }
            }
        }
        if (!adb_path.empty()) {
            g_adb_manager->setAdbPath(adb_path);
            MLOG_INFO("adb", "ADB path set: %s", adb_path.c_str());
        } else {
            MLOG_WARN("adb", "ADB path not found in config, using 'adb' from PATH");
        }
    }
    g_adb_manager->refresh();

    // Signal main thread that ADB is ready (devices are listed, window can be created)
    g_adb_ready.store(true);
    MLOG_INFO("adb", "ADB ready signaled (window creation unblocked)");

    // Remaining device info and X1 initialization continues in background
    auto devices = g_adb_manager->getUniqueDevices();

        // Force X1 max_size periodically (prevents adaptive downscale to 1072 on TCP-only)
        for (const auto& dev : devices) {
            if (dev.display_name.find("Npad X1") != std::string::npos) {
                const std::string adb_id = !dev.wifi_connections.empty() ? dev.wifi_connections[0] : (!dev.usb_connections.empty() ? dev.usb_connections[0] : std::string());
                if (!adb_id.empty()) {
                    const int target_max = ((std::min(dev.screen_width, dev.screen_height) > 1080) || (std::max(dev.screen_width, dev.screen_height) > 1920))
                        ? static_cast<int>(std::max(dev.screen_width, dev.screen_height) * 0.90 + 0.5)
                        : std::max(dev.screen_width, dev.screen_height);
                    g_adb_manager->adbCommand(adb_id, "shell am broadcast -a com.mirage.capture.ACTION_VIDEO_MAXSIZE -p com.mirage.capture --ei max_size " + std::to_string(target_max));
                    g_adb_manager->adbCommand(adb_id, "shell am broadcast -a com.mirage.capture.ACTION_VIDEO_IDR -p com.mirage.capture");
                    MLOG_INFO("watchdog", "Force policy max_size=%d on %s native=%dx%d", target_max, adb_id.c_str(), dev.screen_width, dev.screen_height);
                }
            }
        }

    MLOG_INFO("adb", "%zu 鬮ｯ・ｷ繝ｻ・ｿ郢晢ｽｻ繝ｻ・ｰ鬩搾ｽｵ繝ｻ・ｺ郢晢ｽｻ繝ｻ・ｮ鬩幢ｽ｢隴擾ｽｴ郢晢ｽｻ驛｢譎｢・ｽ・ｰ鬩幢ｽ｢繝ｻ・ｧ郢晢ｽｻ繝ｻ・､鬩幢ｽ｢繝ｻ・ｧ郢晢ｽｻ繝ｻ・ｹ鬩幢ｽ｢繝ｻ・ｧ髯ｷ・ｻ髣鯉ｽｨ繝ｻ・ｽ繝ｻ・､髫ｲ蟷｢・ｽ・ｷ驛｢譎｢・ｽ・ｻ:", devices.size());
    for (const auto& dev : devices) {
        MLOG_INFO("threads", "  - %s [%s] USB:%zu WiFi:%zu IP:%s", dev.display_name.c_str(),
                dev.hardware_id.c_str(),
                dev.usb_connections.size(),
                dev.wifi_connections.size(),
                dev.ip_address.c_str());
    }

    // Log to GUI if available
    auto gui = g_gui;
    if (gui) {
        gui->logInfo(u8"ADB鬮ｫ・ｶ・つ髫ｲ蟷｢・ｽ・ｷ驛｢譎｢・ｽ・ｻ: " + std::to_string(devices.size()) + u8"鬮ｯ・ｷ繝ｻ・ｿ郢晢ｽｻ繝ｻ・ｰ (鬯ｯ・ｩ繝ｻ・･髯晢｣ｰ髮懶ｽ｣繝ｻ・ｽ繝ｻ・､驛｢譎｢・ｽ・ｻ鬮ｮ逕ｻ繝ｻ繝ｻ・ｫ繝ｻ・ｯ郢晢ｽｻ繝ｻ・､鬮ｮ荵晢ｽ・ｹ晢ｽｻ");
        for (const auto& dev : devices) {
            std::string conn_info;
            if (!dev.usb_connections.empty()) conn_info += "USB ";
            if (!dev.wifi_connections.empty()) conn_info += "WiFi ";
            gui->logInfo(u8"  " + dev.display_name + " [" + conn_info + "] IP:" + dev.ip_address);
        }
    }

    MLOG_INFO("adb", "ADB detection pass completed");
  } catch (const std::exception& e) {
    MLOG_ERROR("adb", "adbDetectionThread exception: %s", e.what());
  } catch (...) {
    MLOG_ERROR("adb", "adbDetectionThread unknown exception");
  }
}

// =============================================================================
// Device Update Thread 鬩包ｽｯ繝ｻ・ｶ驛｢譎｢・ｽ・ｻStatic Helpers
// =============================================================================

// 鬩幢ｽ｢繝ｻ・ｧ郢晢ｽｻ繝ｻ・ｹ鬩幢ｽ｢隴趣ｽ｢繝ｻ・ｽ繝ｻ・ｭ鬩幢ｽ｢隴擾ｽｴ郢晢ｽｻ驛｢譎｢・ｽ・ｨ鬩幢ｽ｢隴趣ｽ｢繝ｻ・ｽ繝ｻ・ｬ鬩幢ｽ｢繝ｻ・ｧ郢晢ｽｻ繝ｻ・ｷ鬩幢ｽ｢隴趣ｽ｢繝ｻ・ｽ繝ｻ・ｼ鬩幢ｽ｢隴寂・繝ｻ驛｢譎｢・ｽ・ｻ鬩搾ｽｵ繝ｻ・ｺ髣包ｽｵ隴趣ｽ｢繝ｻ・ｽ髢ｾ・･繝ｻ・ｸ繝ｻ・ｺ郢晢ｽｻ繝ｻ・ｮ鬩幢ｽ｢隴弱・・ｽ・ｼ鬩･繝ｻ・ｨ謚ｵ・ｽ・ｹ隴趣ｽ｢繝ｻ・ｽ繝ｻ・ｼ鬩幢ｽ｢隴趣ｽ｢繝ｻ・｣繝ｻ・ｰ鬮ｯ・ｷ繝ｻ・ｿ鬯ｮ・｢・つ郢晢ｽｻ繝ｻ・ｾ髯ｷ莨夲ｽｽ・ｱ驛｢譎｢・ｽ・ｻAI鬮ｯ・ｷ郢晢ｽｻ繝ｻ・ｽ繝ｻ・ｦ鬯ｨ・ｾ郢晢ｽｻ郢晢ｽｻ
static void updateSlotReceiverFrames(const std::shared_ptr<gui::GuiApplication>& gui) {
    for (int i = 0; i < MAX_SLOTS; i++) {
        if (g_receivers[i]) {
            // Copy #1 eliminated: get_latest_shared_frame returns O(1) shared_ptr
            std::shared_ptr<mirage::SharedFrame> sf_direct;
            if (g_receivers[i]->get_latest_shared_frame(sf_direct) && sf_direct) {
                std::string id = "slot_" + std::to_string(i);
                sf_direct->device_id = id;
                mirage::dispatcher().dispatchSharedFrame(sf_direct);
                // Legacy MirrorFrame for AI code below (no rgba data, metadata only)
                ::gui::MirrorFrame frame;
                frame.width    = sf_direct->width;
                frame.height   = sf_direct->height;
                frame.frame_id = sf_direct->frame_id;
                if (true) {

#ifdef USE_AI
                    if (g_ai_engine && g_ai_enabled) {
                        static bool s_async_started=false;
                        if(!s_async_started){g_ai_engine->setAsyncMode(true);s_async_started=true;}
                        // processFrameAsync removed: dispatchFrame delivers via EventBus/onFrameReady
                        // Push match results to GUI overlays
                        if (gui) {
                            auto matches = g_ai_engine->getLastMatches();
                            if (!matches.empty()) {
                                std::vector<DeviceInfo::MatchOverlay> overlays;
                                overlays.reserve(matches.size());
                                for (const auto& m : matches) {
                                    DeviceInfo::MatchOverlay o;
                                    o.template_id = m.template_id;
                                    o.label = m.label;
                                    o.x = m.x; o.y = m.y;
                                    o.w = m.w; o.h = m.h;
                                    o.score = m.score;
                                    o.color = 0;
                                    overlays.push_back(o);
                                }
                                gui->updateDeviceOverlays(id, overlays);
                            }
                        }
                    }
#endif
                }
            }
        }
    }
}

// USB鬩幢ｽ｢隴擾ｽｴ郢晢ｽｻ驛｢譎｢・ｽ・ｰ鬩幢ｽ｢繝ｻ・ｧ郢晢ｽｻ繝ｻ・､鬩幢ｽ｢繝ｻ・ｧ郢晢ｽｻ繝ｻ・ｹ鬯ｨ・ｾ陷茨ｽｷ繝ｻ・ｽ繝ｻ・ｻ鬯ｯ・ｪ繝ｻ・ｭ郢晢ｽｻ繝ｻ・ｲ鬩幢ｽ｢隴趣ｽ｢繝ｻ・ｽ繝ｻ・ｻ鬩幢ｽ｢隴弱・・ｽ・ｼ鬩･繝ｻ・ｨ謚ｵ・ｽ・ｹ隴趣ｽ｢繝ｻ・ｽ繝ｻ・ｼ鬩幢ｽ｢隴趣ｽ｢繝ｻ・｣繝ｻ・ｰ鬮ｯ・ｷ繝ｻ・ｿ鬯ｮ・｢・つ郢晢ｽｻ繝ｻ・ｾ髯ｷ莨夲ｽｽ・ｱ驛｢譎｢・ｽ・ｻ鬩幢ｽ｢隴乗・・ｽ・ｸ驗呻ｽｫ遶包ｽｧ鬩幢ｽ｢隴弱・ﾂｧ繝ｻ蜿悶渚繝ｻ・ｹ隴擾ｽｴ郢晢ｽｻ驛｢譎｢・ｽ・ｩ/鬩幢ｽ｢隴弱・・ｽ・ｧ繝ｻ・ｭ繝ｻ蜿門旭繝ｻ・ｹ隴擾ｽｶ郢晢ｽｻ繝ｻ蜿厄ｽｨ謚ｵ・ｽ・ｹ繝ｻ・ｧ郢晢ｽｻ繝ｻ・ｷ鬩幢ｽ｢隴趣ｽ｢繝ｻ・ｽ繝ｻ・ｼ鬩幢ｽ｢隴寂・繝ｻ驛｢譎｢・ｽ・ｻ鬯ｩ謳ｾ・ｽ・ｨ郢晢ｽｻ繝ｻ・ｱ鬯ｮ・ｫ繝ｻ・ｪ髯懈瑳・ｺ・ｷ繝ｻ・ｳ繝ｻ・ｩ鬮ｫ・ｴ郢晢ｽｻ繝ｻ・ｽ繝ｻ・ｰ
static void registerAndUpdateUsbDevices(const std::shared_ptr<gui::GuiApplication>& gui) {
    // Update video frames from all USB devices (multi-device mode)
    if (g_hybrid_cmd) {
        auto device_ids = g_hybrid_cmd->get_device_ids();

        // Register all USB devices
        for (const auto& device_id : device_ids) {
            if (g_registered_usb_devices.find(device_id) == g_registered_usb_devices.end()) {
                // Resolve USB serial to hardware_id for device unification
                std::string resolved_id = device_id;
                std::string display_name = "USB:" + device_id.substr(0, std::min(size_t(8), device_id.size()));
                if (g_adb_manager) {
                    std::string hw_id = g_adb_manager->resolveUsbSerial(device_id);
                    if (!hw_id.empty()) {
                        resolved_id = hw_id;
                        ::gui::AdbDeviceManager::UniqueDevice dev_info;
                        if (g_adb_manager->getUniqueDevice(hw_id, dev_info)) {
                            display_name = dev_info.display_name;
                        }
                    }
                }
                // Skip if already registered under resolved hardware_id
                if (g_multi_devices_added.count(resolved_id) || g_registered_usb_devices.count(resolved_id)) {
                    g_registered_usb_devices.insert(device_id); // Mark original as handled
                    continue;
                }
                mirage::dispatcher().registerDevice(resolved_id, display_name, "usb");
                g_registered_usb_devices.insert(device_id);
                g_registered_usb_devices.insert(resolved_id);

                if (!g_main_device_set) {
                    gui->setMainDevice(resolved_id);
                    g_main_device_set = true;
                }
            }
        }

        // Get frames from per-device decoders
        struct FrameUpdate {
            std::string device_id;
            std::shared_ptr<mirage::SharedFrame> sf_frame;  // SharedFrame direct
            uint64_t frames_decoded;
            uint64_t packets_received;
        };
        std::vector<FrameUpdate> frame_updates;

        {
            std::lock_guard<std::mutex> lock(g_usb_decoders_mutex);
            for (auto& [device_id, decoder] : g_usb_decoders) {
                if (!decoder) continue;

                std::shared_ptr<mirage::SharedFrame> sf;
                if (decoder->get_latest_shared_frame(sf) && sf) {
                    FrameUpdate update;
                    update.device_id = device_id;
                    update.sf_frame = std::move(sf);
                    update.frames_decoded = decoder->frames_decoded();
                    update.packets_received = decoder->packets_received();
                    frame_updates.push_back(std::move(update));
                }
            }
        }

        for (auto& update : frame_updates) {
            // Resolve USB serial to hardware_id for device unification
            std::string resolved_id = update.device_id;
            if (g_adb_manager) {
                std::string hw_id = g_adb_manager->resolveUsbSerial(update.device_id);
                if (!hw_id.empty()) resolved_id = hw_id;
            }
            if (g_registered_usb_devices.find(update.device_id) == g_registered_usb_devices.end() &&
                !g_multi_devices_added.count(resolved_id) && !g_registered_usb_devices.count(resolved_id)) {
                std::string display_name = "USB:" + update.device_id.substr(0, std::min(size_t(8), update.device_id.size()));
                if (g_adb_manager) {
                    ::gui::AdbDeviceManager::UniqueDevice dev_info;
                    if (g_adb_manager->getUniqueDevice(resolved_id, dev_info)) {
                        display_name = dev_info.display_name;
                    }
                }
                mirage::dispatcher().registerDevice(resolved_id, display_name, "usb");
                g_registered_usb_devices.insert(update.device_id);
                g_registered_usb_devices.insert(resolved_id);
                if (!g_main_device_set) {
                    gui->setMainDevice(resolved_id);
                    g_main_device_set = true;
                }
            }
            // 鬩幢ｽ｢繝ｻ・ｧ郢晢ｽｻ繝ｻ・ｹ鬩幢ｽ｢隴趣ｽ｢繝ｻ・ｽ繝ｻ・ｩ鬩幢ｽ｢繝ｻ・ｧ郢晢ｽｻ繝ｻ・､鬩幢ｽ｢隴擾ｽｴ郢晢ｽｻ驍ｵ・ｺ郢晢ｽｻ繝ｻ・ｹ隴趣ｽ｢繝ｻ・ｽ繝ｻ・ｳ鬩幢ｽ｢繝ｻ・ｧ郢晢ｽｻ繝ｻ・ｰ鬩幢ｽ｢繝ｻ・ｧ郢晢ｽｻ繝ｻ・ｦ鬩幢ｽ｢繝ｻ・ｧ郢晢ｽｻ繝ｻ・｣鬩幢ｽ｢隴趣ｽ｢繝ｻ・ｽ繝ｻ・ｳ鬩幢ｽ｢隴取得・ｽ・ｳ繝ｻ・ｨ驍ｵ・ｺ郢晢ｽｻPS鬯ｮ・ｫ繝ｻ・ｪ鬮｢・ｧ繝ｻ・ｲ郢晢ｽｻ繝ｻ・ｮ髫ｴ莨夲ｽｽ・ｦ郢晢ｽｻ繝ｻ・ｼ驛｢譎｢・ｽ・ｻ鬯ｩ遨ゑｽｼ諛ｶ・ｽ・ｸ隴乗・・ｽ・ｿ繝ｻ・｣鬯ｯ・ｮ繝ｻ・ｫ髫ｴ竏ｬ繝ｻ・つ陞ｳ螟ｲ・ｽ・ｭ陷ｴ繝ｻ・ｽ・ｽ繝ｻ・ｴ鬮ｫ・ｴ郢晢ｽｻ繝ｻ・ｽ繝ｻ・ｰ驛｢譎｢・ｽ・ｻ驛｢譎｢・ｽ・ｻ
            float fps = 0.0f;
            {
                struct FpsState {
                    uint64_t prev_frames = 0;
                    std::chrono::steady_clock::time_point prev_time = std::chrono::steady_clock::now();
                    float last_fps = 0.0f;
                };
                static std::unordered_map<std::string, FpsState> fps_tracker;
                auto& st = fps_tracker[update.device_id];
                auto now_fps = std::chrono::steady_clock::now();
                double elapsed_sec = std::chrono::duration<double>(now_fps - st.prev_time).count();
                if (elapsed_sec >= 1.0) {
                    uint64_t delta_frames = update.frames_decoded - st.prev_frames;
                    st.last_fps = static_cast<float>(delta_frames / elapsed_sec);
                    st.prev_frames = update.frames_decoded;
                    st.prev_time = now_fps;
                }
                fps = st.last_fps;
            }
            {
                // SharedFrame direct dispatch (no MirrorFrame::rgba copy)
                update.sf_frame->device_id = resolved_id;
                mirage::dispatcher().dispatchSharedFrame(update.sf_frame);
            }
            mirage::dispatcher().dispatchStatus(resolved_id, static_cast<int>(mirage::gui::DeviceStatus::AndroidActive), fps, 0, 0);
        }
    }


    // Note: g_multi_receiver frame delivery is handled by framePollThread via setFrameCallback
    // (gui_init.cpp). Polling get_latest_frame here races with has_new_frame_ flag causing
    // frame starvation. Stats-only update here.
    if (g_multi_receiver && g_multi_receiver->running()) {
        auto stats = g_multi_receiver->getStats();
        for (const auto& ds : stats) {
            if (ds.receiving) {
                mirage::dispatcher().dispatchStatus(ds.hardware_id, static_cast<int>(mirage::gui::DeviceStatus::AndroidActive), ds.fps, 0.0f, ds.bandwidth_mbps);
            } else {
                mirage::dispatcher().dispatchStatus(ds.hardware_id, static_cast<int>(mirage::gui::DeviceStatus::Idle));
            }
        }
    }
}

// TCP鬩幢ｽ｢隴寂或・ｾ・ｭ驛｢譎｢・ｽ・ｧ鬩幢ｽ｢繝ｻ・ｧ郢晢ｽｻ繝ｻ・ｪ鬩幢ｽ｢隴趣ｽ｢繝ｻ・ｽ繝ｻ・ｬ鬩幢ｽ｢繝ｻ・ｧ郢晢ｽｻ繝ｻ・ｷ鬩幢ｽ｢隴趣ｽ｢繝ｻ・ｽ繝ｻ・ｼ鬩幢ｽ｢隴寂・繝ｻ驛｢譎｢・ｽ・ｻ鬩搾ｽｵ繝ｻ・ｺ髣包ｽｵ隴趣ｽ｢繝ｻ・ｽ髢ｾ・･繝ｻ・ｸ繝ｻ・ｺ郢晢ｽｻ繝ｻ・ｮ鬩幢ｽ｢隴弱・・ｽ・ｼ鬩･繝ｻ・ｨ謚ｵ・ｽ・ｹ隴趣ｽ｢繝ｻ・ｽ繝ｻ・ｼ鬩幢ｽ｢隴趣ｽ｢繝ｻ・｣繝ｻ・ｰ鬮ｯ・ｷ繝ｻ・ｿ鬯ｮ・｢・つ郢晢ｽｻ繝ｻ・ｾ髫ｴ莨夲ｽｽ・ｦ郢晢ｽｻ繝ｻ・ｼ驛｢譎｢・ｽ・ｻDB forward鬩幢ｽ｢隴趣ｽ｢繝ｻ・ｽ繝ｻ・｢鬩幢ｽ｢隴趣ｽ｢繝ｻ・ｽ繝ｻ・ｼ鬩幢ｽ｢隴乗・・ｽ・ｼ陞滂ｽｲ繝ｻ・ｽ繝ｻ・ｼ驛｢譎｢・ｽ・ｻ
static void updateTcpReceiverFrames([[maybe_unused]] const std::shared_ptr<gui::GuiApplication>& gui) {
}

// =============================================================================
// Device Update Thread
// =============================================================================

void deviceUpdateThread() {
  try {
    MLOG_INFO("threads", "deviceUpdateThread STARTED");
    auto lastStatsTime = std::chrono::steady_clock::now();
    bool early_registration_done = false;

    while (g_running) {
        // Thread-safe local copy of shared_ptr
        auto gui = g_gui;
        if (!gui) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }

        // =====================================================================
        // Early device registration (once, after main loop has started frames)
        // =====================================================================
        if (!early_registration_done && g_adb_manager) {
            // Wait a bit to ensure main loop has called beginFrame at least once
            std::this_thread::sleep_for(std::chrono::milliseconds(500));

            auto devices = g_adb_manager->getUniqueDevices();
            for (const auto& dev : devices) {
                if (!g_multi_devices_added[dev.hardware_id]) {
                    gui->addDevice(dev.hardware_id, dev.display_name);
                    mirage::dispatcher().registerDevice(dev.hardware_id, dev.display_name, "adb");
                    g_multi_devices_added[dev.hardware_id] = true;
                    MLOG_INFO("threads", "Early device registration: %s [%s]",
                             dev.display_name.c_str(), dev.hardware_id.c_str());

                    if (!g_main_device_set) {
                        gui->setMainDevice(dev.hardware_id);
                        g_main_device_set = true;
                        MLOG_INFO("threads", "Set main device: %s", dev.hardware_id.c_str());
                    }
                }
            }

            // Set status to Idle (waiting for video)
            for (const auto& dev : devices) {
                mirage::dispatcher().dispatchStatus(dev.hardware_id,
                    static_cast<int>(mirage::gui::DeviceStatus::Idle));
            }

            early_registration_done = true;

        }

        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastStatsTime).count();
        
        // Poll stats every 500ms
        if (elapsed >= 500) {
            auto slots = fetchSlotStats();

            for (const auto& slot : slots) {
                std::string id = "slot_" + std::to_string(slot.slot);

                // Add device if new
                if (!g_slot_active[slot.slot]) {
                    mirage::dispatcher().registerDevice(id, slot.serial, "slot");
                    g_slot_active[slot.slot] = true;

                    // Start video receiver
                    uint16_t port = calculatePort(slot.serial, g_device_channel_count[slot.serial]++);
                    g_receivers[slot.slot] = std::make_unique<::gui::MirrorReceiver>();
                    g_receivers[slot.slot]->start(port);

                    gui->logInfo(u8"鬩幢ｽ｢隴擾ｽｴ郢晢ｽｻ驛｢譎｢・ｽ・ｰ鬩幢ｽ｢繝ｻ・ｧ郢晢ｽｻ繝ｻ・､鬩幢ｽ｢繝ｻ・ｧ郢晢ｽｻ繝ｻ・ｹ鬮ｫ・ｰ隴会ｽｦ繝ｻ・ｽ繝ｻ・･鬯ｩ謳ｾ・ｽ・ｯ驛｢譎｢・ｽ・ｻ " + slot.serial + " (鬩幢ｽ｢繝ｻ・ｧ郢晢ｽｻ繝ｻ・ｹ鬩幢ｽ｢隴趣ｽ｢繝ｻ・ｽ繝ｻ・ｭ鬩幢ｽ｢隴擾ｽｴ郢晢ｽｻ驛｢譎｢・ｽ・ｨ" + std::to_string(slot.slot) + ")");
                }

                // Update status based on KPIs
                mirage::gui::DeviceStatus status;
                if (slot.tx_err > 10 || slot.disc > 0) {
                    status = mirage::gui::DeviceStatus::Stuck;
                } else if (slot.tx_err > 0 || slot.ack_to > 0) {
                    status = mirage::gui::DeviceStatus::Error;
                } else {
                    status = mirage::gui::DeviceStatus::AndroidActive;
                }

                mirage::dispatcher().dispatchStatus(id, static_cast<int>(status), 30.0f, static_cast<float>(slot.ack_to), 0.0f);
            }

            // Check for disconnected slots
            for (int i = 0; i < MAX_SLOTS; i++) {
                if (g_slot_active[i]) {
                    bool found = false;
                    for (const auto& slot : slots) {
                        if (slot.slot == i) {
                            found = true;
                            break;
                        }
                    }
                    if (!found) {
                        std::string id = "slot_" + std::to_string(i);
                        mirage::dispatcher().dispatchStatus(id, static_cast<int>(mirage::gui::DeviceStatus::Disconnected));
                        g_slot_active[i] = false;

                        if (g_receivers[i]) {
                            g_receivers[i]->stop();
                            g_receivers[i].reset();
                        }

                        gui->logWarning(u8"鬩幢ｽ｢隴擾ｽｴ郢晢ｽｻ驛｢譎｢・ｽ・ｰ鬩幢ｽ｢繝ｻ・ｧ郢晢ｽｻ繝ｻ・､鬩幢ｽ｢繝ｻ・ｧ郢晢ｽｻ繝ｻ・ｹ鬮ｯ蜈ｷ・ｽ・ｻ驛｢譎｢・ｽ・ｻ髯橸ｽｯ郢晢ｽｻ 鬩幢ｽ｢繝ｻ・ｧ郢晢ｽｻ繝ｻ・ｹ鬩幢ｽ｢隴趣ｽ｢繝ｻ・ｽ繝ｻ・ｭ鬩幢ｽ｢隴擾ｽｴ郢晢ｽｻ驛｢譎｢・ｽ・ｨ" + std::to_string(i));
                    }
                }
            }

            lastStatsTime = now;
        }

        updateSlotReceiverFrames(gui);
        registerAndUpdateUsbDevices(gui);
        updateTcpReceiverFrames(gui);

        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }
  } catch (const std::exception& e) {
    MLOG_ERROR("threads", "deviceUpdateThread exception: %s", e.what());
  } catch (...) {
    MLOG_ERROR("threads", "deviceUpdateThread unknown exception");
  }
}


// =============================================================================
// WiFi ADB Watchdog Thread
// =============================================================================

void wifiAdbWatchdogThread() {
  try {
    MLOG_INFO("watchdog", "WiFi ADB watchdog started (15s interval)");

    while (g_running) {
        for (int i = 0; i < 15 && g_running; i++) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        if (!g_running) break;

        if (!g_adb_manager) continue;

        g_adb_manager->refresh();
        auto devices = g_adb_manager->getUniqueDevices();

        for (const auto& dev : devices) {
            if (!dev.usb_connections.empty() && dev.wifi_connections.empty()) {
                MLOG_INFO("watchdog", "Device %s has USB but no WiFi ADB - enabling...",
                         dev.display_name.c_str());

                const auto& usb_id = dev.usb_connections[0];
                std::string result = g_adb_manager->adbCommand(usb_id, "tcpip 5555");
                MLOG_INFO("watchdog", "tcpip 5555 on %s: %s", usb_id.c_str(), result.c_str());

                std::this_thread::sleep_for(std::chrono::seconds(2));

                if (!dev.ip_address.empty()) {
                    std::string connect_cmd = "adb connect " + dev.ip_address + ":5555";
                    MLOG_INFO("watchdog", "Executing: %s", connect_cmd.c_str());
                    int ret = execHidden(connect_cmd);
                    MLOG_INFO("watchdog", "adb connect %s:5555 returned %d",
                             dev.ip_address.c_str(), ret);

                    std::this_thread::sleep_for(std::chrono::seconds(1));
                    g_adb_manager->refresh();
                }
            }

            // A) WiFi ADB鬮ｫ・ｰ隴会ｽｦ繝ｻ・ｽ繝ｻ・･鬯ｩ謳ｾ・ｽ・ｯ髯橸ｽ｢繝ｻ・ｹ驛｢譎｢・ｽ・ｧ鬩幢ｽ｢隴寂・繝ｻ驍ｵ・ｺ郢晢ｽｻ繝ｻ・ｹ繝ｻ・ｧ郢晢ｽｻ繝ｻ・ｹ鬩搾ｽｵ繝ｻ・ｺ郢晢ｽｻ繝ｻ・ｮA11y鬯ｮ・｢繝ｻ・ｾ郢晢ｽｻ繝ｻ・ｪ鬮ｯ・ｷ髢ｧ・ｴ郢晢ｽｻ郢晢ｽｻ繝ｻ・ｨ郢晢ｽｻ繝ｻ・ｭ鬮ｯ讖ｸ・ｽ・ｳ髯樊ｻゑｽｽ・ｲ郢晢ｽｻ繝ｻ・ｼ驛｢譎｢・ｽ・ｻ1鬮ｯ・ｷ繝ｻ・ｷ郢晢ｽｻ繝ｻ・ｫ鬩幢ｽ｢繝ｻ・ｧ繝ｻ縺､ﾂ驛｢譎｢・ｽ・ｻ驛｢譎｢・ｽ・ｻ
            if (!dev.wifi_connections.empty()) {
                const auto& wifi_id = dev.wifi_connections[0];
                std::string a11y = g_adb_manager->adbCommand(wifi_id,
                    "shell settings get secure enabled_accessibility_services");
                if (a11y.find("MirageAccessibilityService") == std::string::npos) {
                    g_adb_manager->adbCommand(wifi_id,
                        "shell settings put secure enabled_accessibility_services "
                        "com.mirage.capture/.access.MirageAccessibilityService");
                    g_adb_manager->adbCommand(wifi_id,
                        "shell settings put secure accessibility_enabled 1");
                    MLOG_INFO("watchdog", "A11y re-enabled on %s", dev.display_name.c_str());
                }
            }

            // B) Npad X1鬩搾ｽｵ繝ｻ・ｺ郢晢ｽｻ繝ｻ・ｮmax_size鬩幢ｽ｢隴弱・ﾂｧ繝ｻ蜿厄ｽｺ・ｽ繝ｻ・ｹ隴趣ｽ｢繝ｻ・ｽ繝ｻ・ｼ鬩幢ｽ｢隴取得・ｽ・ｳ繝ｻ・ｨ驍ｵ・ｺ陷證ｦ・ｽ・ｹ隴趣ｽ｢繝ｻ・ｽ繝ｻ・｣鬩幢ｽ｢繝ｻ・ｧ郢晢ｽｻ繝ｻ・ｹ鬩幢ｽ｢隴寂握縺狗ｹ晢ｽｻ繝ｻ・ｼ驛｢譎｢・ｽ・ｻdaptive downscale鬯ｯ・ｮ繝ｻ・ｦ郢晢ｽｻ繝ｻ・ｲ鬮ｮ蠑ｱ繝ｻ繝ｻ・ｽ繝ｻ・｢驛｢譎｢・ｽ・ｻ驛｢譎｢・ｽ・ｻ
            if (!dev.wifi_connections.empty()) {
                const auto& wifi_id = dev.wifi_connections[0];
                const int target_max = ((std::min(dev.screen_width, dev.screen_height) > 1080) || (std::max(dev.screen_width, dev.screen_height) > 1920))
                    ? static_cast<int>(std::max(dev.screen_width, dev.screen_height) * 0.90 + 0.5)
                    : std::max(dev.screen_width, dev.screen_height);
                g_adb_manager->adbCommand(wifi_id,
                    "shell am broadcast -a com.mirage.capture.ACTION_VIDEO_MAXSIZE "
                    "-p com.mirage.capture --ei max_size " + std::to_string(target_max));
                MLOG_INFO("watchdog", "Force policy max_size=%d on %s native=%dx%d", target_max, wifi_id.c_str(), dev.screen_width, dev.screen_height);
            }

            // C) ScreenCaptureService 鬮ｮ蠑ｱ繝ｻ繝ｻ・ｽ繝ｻ・ｻ鬮ｮ雜｣・ｽ・｢郢晢ｽｻ繝ｻ・ｻ鬯ｨ・ｾ繝ｻ・ｶ郢晢ｽｻ繝ｻ・｣鬯ｮ・ｫ髴域鱒繝ｻ
            if (!dev.wifi_connections.empty()) {
                const auto& wifi_id = dev.wifi_connections[0];
                std::string svc = g_adb_manager->adbCommand(wifi_id,
                    "shell dumpsys activity services com.mirage.capture");
                if (svc.find("ScreenCaptureService") == std::string::npos) {
                    MLOG_INFO("watchdog", "ScreenCaptureService dead on %s, restarting...",
                              dev.display_name.c_str());
                    g_adb_manager->adbCommand(wifi_id,
                        "shell am start -n com.mirage.capture/.ui.CaptureActivity "
                        "--ez auto_mirror true --es mirror_mode tcp --ei tcp_port " + std::to_string(dev.assigned_tcp_port > 0 ? dev.assigned_tcp_port : 50100));
                    // Force FPS re-send after service restart
                    if (g_route_controller) g_route_controller->resetDeviceFps(dev.hardware_id);
                }
            }
        }

        // D) MultiReceiver鬮ｫ・ｴ陝ｷ・｢繝ｻ・ｽ繝ｻ・ｪ鬮ｯ蜈ｷ・ｽ・ｻ髫ｴ蠑ｱ繝ｻ繝ｻ繝ｻ蛻ｹ繝ｻ・ｹ髫ｰ雋ｻ・ｽ・ｶ驍ｵ・ｲ陜｣・､繝ｻ・ｹ隴擾ｽｴ郢晢ｽｻ驛｢譎｢・ｽ・ｰ鬩幢ｽ｢繝ｻ・ｧ郢晢ｽｻ繝ｻ・､鬩幢ｽ｢繝ｻ・ｧ郢晢ｽｻ繝ｻ・ｹ鬩搾ｽｵ繝ｻ・ｺ髫ｰ逍ｲ・ｻ繧托ｽｽ・ｽ繝ｻ・ｭ髣費ｽｨ隲帛､ｷ蟶昴＠繝ｻ・ｺ髯ｷ・ｷ繝ｻ・ｶ郢晢ｽｻ驍・戟謐礼ｹ晢ｽｻ繝ｻ・ｴ鬮ｯ・ｷ繝ｻ・ｷ髯具ｽｹ繝ｻ・ｻ驛｢譎｢・ｽ・ｻ鬮ｯ・ｷ・つ髫ｶ螳茨ｽｿ・ｫ郢晢ｽｻ鬮ｫ・ｴ陝ｶ・ｶ繝ｻ・ｺ繝ｻ・ｷ髯懈ｻゑｽｽ・ｧ
        if (!g_multi_receiver && !devices.empty()) {
            MLOG_INFO("watchdog", "MultiReceiver uninitialized but %zu device(s) found 鬩包ｽｯ繝ｻ・ｶ驛｢譎｢・ｽ・ｻre-initializing",
                      devices.size());
            mirage::gui::init::initializeMultiReceiver();
        }
    }
    MLOG_INFO("watchdog", "WiFi ADB watchdog stopped");
  } catch (const std::exception& e) {
    MLOG_ERROR("watchdog", "wifiAdbWatchdogThread exception: %s", e.what());
  } catch (...) {
    MLOG_ERROR("watchdog", "wifiAdbWatchdogThread unknown exception");
  }
}

} // namespace mirage::gui::threads

