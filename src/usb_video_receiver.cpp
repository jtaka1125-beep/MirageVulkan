#include "usb_video_receiver.hpp"
#include <cstdio>
#include <cstring>
#include <chrono>
#include <stdexcept>
#include <vector>
#include <algorithm>
#include <system_error>
#include "mirage_log.hpp"
#include "mirage_protocol.hpp"
using namespace mirage::protocol;

namespace gui {

UsbVideoReceiver::UsbVideoReceiver() : ring_(RING_BUFFER_SIZE, 0) {}
UsbVideoReceiver::~UsbVideoReceiver() { stop(); }

// === Ring buffer O(1) ops ===

void UsbVideoReceiver::ring_write(const uint8_t* data, size_t len) {
    if (!len) return;
    size_t space = RING_BUFFER_SIZE - ring_available() - 1;
    if (len > space) {
        ring_skip(len - space);
        drop_count_++;
        if (drop_count_ <= 10 || drop_count_ % 100 == 0)
            MLOG_INFO("usbvid", "Ring overflow discard (drops=%llu)", (unsigned long long)drop_count_);
    }
    size_t first = std::min(len, RING_BUFFER_SIZE - ring_head_);
    memcpy(ring_.data() + ring_head_, data, first);
    if (len > first) memcpy(ring_.data(), data + first, len - first);
    ring_head_ = (ring_head_ + len) % RING_BUFFER_SIZE;
}

void UsbVideoReceiver::ring_read(uint8_t* dst, size_t len) {
    size_t first = std::min(len, RING_BUFFER_SIZE - ring_tail_);
    memcpy(dst, ring_.data() + ring_tail_, first);
    if (len > first) memcpy(dst + first, ring_.data(), len - first);
    ring_tail_ = (ring_tail_ + len) % RING_BUFFER_SIZE;
}

void UsbVideoReceiver::ring_skip(size_t len) {
    ring_tail_ = (ring_tail_ + len) % RING_BUFFER_SIZE;
}

uint8_t UsbVideoReceiver::ring_peek(size_t offset) const {
    return ring_[(ring_tail_ + offset) % RING_BUFFER_SIZE];
}

size_t UsbVideoReceiver::ring_contiguous_from_tail() const {
    if (ring_head_ >= ring_tail_) return ring_head_ - ring_tail_;
    return RING_BUFFER_SIZE - ring_tail_;
}

bool UsbVideoReceiver::ring_find_magic(size_t& offset_out) const {
    size_t avail = ring_available();
    if (avail < 4) return false;
    for (size_t i = 0; i + 3 < avail; ) {
        size_t pos = (ring_tail_ + i) % RING_BUFFER_SIZE;
        size_t contig = std::min(RING_BUFFER_SIZE - pos, avail - i);
        if (contig >= 4) {
            const uint8_t* base = ring_.data() + pos;
            size_t slen = contig - 3;
            for (size_t j = 0; j < slen; ) {
                const uint8_t* f = (const uint8_t*)memchr(base + j, 0x56, slen - j);
                if (!f) { j = slen; break; }
                size_t idx = f - base;
                if (idx + 3 < contig && f[1]==0x49 && f[2]==0x44 && f[3]==0x30) {
                    offset_out = i + idx;
                    return true;
                }
                j = idx + 1;
            }
            i += slen;
        } else {
            if (ring_peek(i)==0x56 && ring_peek(i+1)==0x49 && ring_peek(i+2)==0x44 && ring_peek(i+3)==0x30) {
                offset_out = i;
                return true;
            }
            i++;
        }
    }
    return false;
}

bool UsbVideoReceiver::is_sps_pps_rtp(const uint8_t* data, size_t len) {
    if (len < 13) return false;
    uint8_t nt = data[12] & 0x1F;
    if (nt == 7 || nt == 8) return true;
    if (nt == 24 && len >= 16) { uint8_t fn = data[15] & 0x1F; if (fn==7||fn==8) return true; }
    return false;
}

#ifdef USE_LIBUSB

// === Async transfers ===

void LIBUSB_CALL UsbVideoReceiver::transfer_callback(libusb_transfer* xfer) {
    auto* slot = static_cast<TransferSlot*>(xfer->user_data);
    auto* self = slot->owner;
    if (xfer->status == LIBUSB_TRANSFER_COMPLETED) {
        if (xfer->actual_length > 0) {
            self->bytes_received_.fetch_add(xfer->actual_length);
            std::lock_guard<std::mutex> lk(self->ring_mtx_);
            self->ring_write(slot->buffer, xfer->actual_length);
        }
        // Zero-length completed: normal, just resubmit
    } else if (xfer->status == LIBUSB_TRANSFER_CANCELLED) {
        return;
    } else if (xfer->status != LIBUSB_TRANSFER_TIMED_OUT) {
        MLOG_ERROR("usbvid", "Async xfer error: %d", xfer->status);
        self->connected_.store(false);
        return;
    }
    if (self->running_.load()) {
        if (libusb_submit_transfer(xfer) != LIBUSB_SUCCESS) {
            self->connected_.store(false);
        }
    }
}

bool UsbVideoReceiver::setup_async_transfers() {
    for (int i = 0; i < NUM_TRANSFERS; i++) {
        transfers_[i].owner = this;
        transfers_[i].transfer = libusb_alloc_transfer(0);
        if (!transfers_[i].transfer) return false;
        libusb_fill_bulk_transfer(transfers_[i].transfer, handle_, ep_in_,
            transfers_[i].buffer, USB_BUFFER_SIZE, transfer_callback, &transfers_[i], USB_TIMEOUT_MS);
        if (libusb_submit_transfer(transfers_[i].transfer) != LIBUSB_SUCCESS) return false;
    }
    MLOG_INFO("usbvid", "%d async transfers submitted", NUM_TRANSFERS);
    return true;
}

void UsbVideoReceiver::cancel_async_transfers() {
    for (int i = 0; i < NUM_TRANSFERS; i++)
        if (transfers_[i].transfer) libusb_cancel_transfer(transfers_[i].transfer);
    struct timeval tv = {0, 200000};
    for (int i = 0; i < 30; i++) libusb_handle_events_timeout(ctx_, &tv);
    for (int i = 0; i < NUM_TRANSFERS; i++) {
        if (transfers_[i].transfer) { libusb_free_transfer(transfers_[i].transfer); transfers_[i].transfer = nullptr; }
    }
}

// === Start/Stop ===

bool UsbVideoReceiver::start() {
    if (running_.load()) return true;
    if (libusb_init(&ctx_) != LIBUSB_SUCCESS) return false;
    if (!find_and_open_device()) { libusb_exit(ctx_); ctx_=nullptr; return false; }
    ring_head_ = ring_tail_ = 0;
    running_.store(true); connected_.store(true);
    try { thread_ = std::thread(&UsbVideoReceiver::receive_thread, this); }
    catch (const std::system_error& e) {
        running_.store(false); connected_.store(false);
        libusb_release_interface(handle_,0); libusb_close(handle_); handle_=nullptr;
        libusb_exit(ctx_); ctx_=nullptr; return false;
    }
    MLOG_INFO("usbvid", "Started v3 (ring=%zuKB async=%d flush=%dms)", RING_BUFFER_SIZE/1024, NUM_TRANSFERS, FLUSH_PERIOD_MS);
    return true;
}

void UsbVideoReceiver::stop() {
    running_.store(false); connected_.store(false);
    if (thread_.joinable()) thread_.join();
    if (handle_) { libusb_release_interface(handle_,0); libusb_close(handle_); handle_=nullptr; }
    if (ctx_) { libusb_exit(ctx_); ctx_=nullptr; }
}

// === Device discovery ===

bool UsbVideoReceiver::find_and_open_device() {
    uint16_t aoa_pids[] = { AOA_PID_ACCESSORY, AOA_PID_ACCESSORY_ADB,
                            AOA_PID_ACCESSORY_AUDIO, AOA_PID_ACCESSORY_AUDIO_ADB };
    libusb_device** devs;
    ssize_t cnt = libusb_get_device_list(ctx_, &devs);
    if (cnt < 0) return false;

    struct AoaDev { libusb_device* dev; uint8_t bus; uint8_t addr; };
    std::vector<AoaDev> aoa_devs;
    for (ssize_t i = 0; i < cnt; i++) {
        libusb_device_descriptor desc;
        if (libusb_get_device_descriptor(devs[i], &desc) != 0) continue;
        if (desc.idVendor != AOA_VID) continue;
        bool ok = false;
        for (auto pid : aoa_pids) if (desc.idProduct == pid) { ok=true; break; }
        if (ok) aoa_devs.push_back({devs[i], libusb_get_bus_number(devs[i]), libusb_get_device_address(devs[i])});
    }
    MLOG_INFO("usbvid", "AOA devices: %d (serial=%s idx=%d)", (int)aoa_devs.size(),
              target_serial_.empty()?"any":target_serial_.c_str(), device_index_);

    int idx = 0;
    for (auto& ad : aoa_devs) {
        if (device_index_ >= 0 && idx != device_index_) { idx++; continue; }
        if (libusb_open(ad.dev, &handle_) != LIBUSB_SUCCESS) { idx++; continue; }
        libusb_device_descriptor desc;
        libusb_get_device_descriptor(ad.dev, &desc);
        char serial[256]={0};
        if (desc.iSerialNumber)
            libusb_get_string_descriptor_ascii(handle_, desc.iSerialNumber, (unsigned char*)serial, sizeof(serial));
        if (!target_serial_.empty() && target_serial_ != serial) {
            libusb_close(handle_); handle_=nullptr; idx++; continue;
        }
        if (libusb_claim_interface(handle_, 0) != LIBUSB_SUCCESS) {
            libusb_close(handle_); handle_=nullptr; idx++; continue;
        }
        ep_in_ = 0;
        struct libusb_config_descriptor* config;
        if (libusb_get_active_config_descriptor(libusb_get_device(handle_), &config) == LIBUSB_SUCCESS) {
            for (int j = 0; j < config->interface[0].altsetting[0].bNumEndpoints; j++) {
                const auto* ep = &config->interface[0].altsetting[0].endpoint[j];
                if ((ep->bEndpointAddress & LIBUSB_ENDPOINT_DIR_MASK) == LIBUSB_ENDPOINT_IN &&
                    (ep->bmAttributes & LIBUSB_TRANSFER_TYPE_MASK) == LIBUSB_TRANSFER_TYPE_BULK) {
                    ep_in_ = ep->bEndpointAddress; break;
                }
            }
            libusb_free_config_descriptor(config);
        }
        if (!ep_in_) { libusb_release_interface(handle_,0); libusb_close(handle_); handle_=nullptr; idx++; continue; }
        MLOG_INFO("usbvid", "Selected [%d:%d] ep=0x%02x serial=%s", ad.bus, ad.addr, ep_in_, serial[0]?serial:"?");
        libusb_free_device_list(devs, 1);
        return true;
    }
    libusb_free_device_list(devs, 1);
    return false;
}

// === Receive thread ===

void UsbVideoReceiver::receive_thread() {
    MLOG_INFO("usbvid", "Receive thread started");
    bool use_async = setup_async_transfers();
    if (!use_async) MLOG_INFO("usbvid", "Async failed, sync fallback");

    auto t0 = std::chrono::steady_clock::now();
    bool flushing = true;
    uint64_t flush_sps = 0;
    struct timeval tv = {0, 5000};

    while (running_.load()) {
        if (use_async) {
            libusb_handle_events_timeout(ctx_, &tv);
        } else {
            uint8_t buf[USB_BUFFER_SIZE]; int xfrd;
            int ret = libusb_bulk_transfer(handle_, ep_in_, buf, USB_BUFFER_SIZE, &xfrd, USB_TIMEOUT_MS);
            if (ret == LIBUSB_SUCCESS && xfrd > 0) {
                bytes_received_.fetch_add(xfrd);
                std::lock_guard<std::mutex> lk(ring_mtx_);
                ring_write(buf, xfrd);
            } else if (ret != LIBUSB_SUCCESS && ret != LIBUSB_ERROR_TIMEOUT) {
                connected_.store(false); break;
            }
        }

        std::lock_guard<std::mutex> lk(ring_mtx_);
        if (flushing) {
            auto dt = std::chrono::steady_clock::now() - t0;
            if (dt > std::chrono::milliseconds(FLUSH_PERIOD_MS)) {
                flushing = false;
                MLOG_INFO("usbvid", "Flush done (SPS/PPS passed: %llu)", (unsigned long long)flush_sps);
            }
            process_ring_flush(flush_sps);
        } else {
            process_ring();
        }
    }

    if (use_async) cancel_async_transfers();
    MLOG_INFO("usbvid", "Thread ended (pkts=%llu bytes=%llu drops=%llu sync_err=%llu)",
              (unsigned long long)packets_received_.load(), (unsigned long long)bytes_received_.load(),
              (unsigned long long)drop_count_, (unsigned long long)sync_errors_);
}

// === Normal packet extraction ===

void UsbVideoReceiver::process_ring() {
    static thread_local std::vector<uint8_t> pkt;
    while (ring_available() >= 8) {
        uint32_t magic = (uint32_t(ring_peek(0))<<24)|(uint32_t(ring_peek(1))<<16)|
                         (uint32_t(ring_peek(2))<<8)|uint32_t(ring_peek(3));
        if (magic != USB_VIDEO_MAGIC) {
            sync_errors_++;
            size_t off;
            if (ring_find_magic(off) && off > 0) { ring_skip(off); continue; }
            size_t a = ring_available(); if (a>3) ring_skip(a-3);
            break;
        }
        uint32_t len = (uint32_t(ring_peek(4))<<24)|(uint32_t(ring_peek(5))<<16)|
                       (uint32_t(ring_peek(6))<<8)|uint32_t(ring_peek(7));
        if (len > MAX_PACKET_LEN || len < MIN_PACKET_LEN) { ring_skip(1); sync_errors_++; continue; }
        if (ring_available() < 8+len) break;
        ring_skip(8);
        pkt.resize(len);
        ring_read(pkt.data(), len);
        if (rtp_callback_) rtp_callback_(pkt.data(), len);
        packets_received_.fetch_add(1);
    }
}

// === Flush mode: SPS/PPS passthrough only ===

void UsbVideoReceiver::process_ring_flush(uint64_t& sps_cnt) {
    static thread_local std::vector<uint8_t> pkt;
    while (ring_available() >= 8) {
        uint32_t magic = (uint32_t(ring_peek(0))<<24)|(uint32_t(ring_peek(1))<<16)|
                         (uint32_t(ring_peek(2))<<8)|uint32_t(ring_peek(3));
        if (magic != USB_VIDEO_MAGIC) {
            size_t off;
            if (ring_find_magic(off) && off > 0) { ring_skip(off); continue; }
            size_t a = ring_available(); if (a>3) ring_skip(a-3);
            break;
        }
        uint32_t len = (uint32_t(ring_peek(4))<<24)|(uint32_t(ring_peek(5))<<16)|
                       (uint32_t(ring_peek(6))<<8)|uint32_t(ring_peek(7));
        if (len > MAX_PACKET_LEN || len < MIN_PACKET_LEN) { ring_skip(1); continue; }
        if (ring_available() < 8+len) break;
        ring_skip(8);
        pkt.resize(len);
        ring_read(pkt.data(), len);
        if (is_sps_pps_rtp(pkt.data(), len)) {
            if (rtp_callback_) rtp_callback_(pkt.data(), len);
            sps_cnt++;
            packets_received_.fetch_add(1);
        }
    }
}

#else
bool UsbVideoReceiver::start() { MLOG_INFO("usbvid","No libusb"); return false; }
void UsbVideoReceiver::stop() { running_.store(false); }
#endif

} // namespace gui
