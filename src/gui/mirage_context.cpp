// =============================================================================
// MirageSystem v2 - MirageContext Implementation
// =============================================================================
#include "mirage_context.hpp"
#include "mirage_log.hpp"

namespace mirage {

MirageContext::MirageContext() = default;

MirageContext::~MirageContext() {
    shutdown();
}

void MirageContext::initialize() {
    running = true;
    adb_ready = false;
    main_device_set = false;
    fallback_device_added = false;
    slot_active.fill(false);
    registered_usb_devices.clear();
    multi_devices_added.clear();
}

void MirageContext::shutdown() {
    // Stop all receivers
    for (int i = 0; i < MAX_SLOTS; i++) {
        if (receivers[i]) {
            receivers[i]->stop();
            receivers[i].reset();
        }
    }

    // Cleanup hybrid receivers
    if (hybrid_receiver) {
        hybrid_receiver->stop();
        hybrid_receiver.reset();
    }
    if (hybrid_cmd) {
        hybrid_cmd->stop();
        hybrid_cmd.reset();
    }

    // Cleanup multi-device receiver
    if (multi_receiver) {
        multi_receiver->stop();
        multi_receiver.reset();
    }

    // Cleanup routing
    route_eval_running.store(false);
    if (route_eval_thread.joinable()) {
        route_eval_thread.join();
    }
    route_controller.reset();
    bandwidth_monitor.reset();

    // Cleanup USB video receiver
    if (usb_video_receiver) {
        usb_video_receiver->stop();
        usb_video_receiver.reset();
    }

    // Cleanup USB decoders
    {
        std::lock_guard<std::mutex> lock(usb_decoders_mutex);
        usb_decoders.clear();
    }

    // Shutdown AI
#ifdef USE_AI
    if (ai_engine) {
        ai_engine->shutdown();
        ai_engine.reset();
    }
#endif

    // Shutdown OCR
#ifdef USE_OCR
    if (ocr_engine) {
        ocr_engine->shutdown();
        ocr_engine.reset();
    }
#endif

    // Shutdown GUI
    if (gui) {
        gui->shutdown();
        gui.reset();
    }

    // Cleanup remaining
    ipc.reset();
    adb_manager.reset();
}

MirageContext& ctx() {
    static MirageContext instance;
    return instance;
}

} // namespace mirage
