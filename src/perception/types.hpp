// =============================================================================
// MirageSystem - Perception Layer Types
// =============================================================================
// Common types shared between Macro Engine and AI Layer.
//
// Design principles:
//   - perception::detect() / matchTemplate() / ocr() are PULL-based.
//     Callers invoke them when they need data; no push/subscription here.
//   - Frame::data is a non-owning view (std::span).
//     Caller must guarantee the backing buffer remains valid for the
//     duration of synchronous perception calls such as detect() /
//     matchTemplate().
//   - PerceptionHistory (AI side) copies data it needs to retain beyond
//     the call. MacroEngine does NOT retain frames.
//   - dedup is NOT performed here. MacroEngine and AI Layer apply their
//     own dedup strategies suited to their use cases.
// =============================================================================

#pragma once
#include <span>
#include <string>
#include <vector>
#include <cstdint>

namespace mirage::perception {

// ---------------------------------------------------------------------------
// PixelFormat
// ---------------------------------------------------------------------------
enum class PixelFormat {
    kJPEG,   // Compressed JPEG (most common from AiJpegReceiver)
    kNV21,   // YUV 4:2:0 semi-planar (Android default camera)
    kRGBA,   // 32-bit RGBA (Vulkan render output)
};

// ---------------------------------------------------------------------------
// Frame
// ---------------------------------------------------------------------------
// A non-owning view of a single decoded or compressed video frame.
//
// LIFETIME CONTRACT:
//   data must remain valid for the full duration of any synchronous
//   perception call that receives this Frame.
//   Asynchronous consumers (e.g. PerceptionHistory) must copy the bytes.
//
// frame_id is monotonically increasing per device.
// timestamp_ms is wall-clock milliseconds (steady_clock epoch).
// ---------------------------------------------------------------------------
struct Frame {
    std::span<const uint8_t> data;  // non-owning view of compressed/raw bytes
    int width        = 0;
    int height       = 0;
    PixelFormat format = PixelFormat::kJPEG;
    uint64_t frame_id      = 0;
    int64_t  timestamp_ms  = 0;
};

// ---------------------------------------------------------------------------
// FrameInfo
// ---------------------------------------------------------------------------
// Lightweight metadata extracted from a Frame.
// Stored inside UiElementHit so callers can check freshness without
// retaining the full frame buffer.
// ---------------------------------------------------------------------------
struct FrameInfo {
    uint64_t frame_id     = 0;
    int64_t  timestamp_ms = 0;
    int width  = 0;
    int height = 0;
};

inline FrameInfo frameInfoOf(const Frame& f) {
    return { f.frame_id, f.timestamp_ms, f.width, f.height };
}

// ---------------------------------------------------------------------------
// Rect
// ---------------------------------------------------------------------------
struct Rect {
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;

    int cx() const { return x + w / 2; }
    int cy() const { return y + h / 2; }
    bool empty() const { return w <= 0 || h <= 0; }
};

// ---------------------------------------------------------------------------
// UiElementHit
// ---------------------------------------------------------------------------
// A single detected UI element (button, icon, text region, template match).
// Returned by detect(), matchTemplate(), ocr().
//
// NOTE: dedup is NOT applied here. Callers apply their own strategy.
// ---------------------------------------------------------------------------
struct UiElementHit {
    std::string type;       // "button" / "icon" / "text" / "template"
    std::string label;      // detected text or template name
    Rect        bounds;
    float       confidence  = 0.0f;
    FrameInfo   frame_info;  // origin frame metadata for freshness checks
};

// ---------------------------------------------------------------------------
// TemplateMatchResult
// ---------------------------------------------------------------------------
// Result of a single template matching operation.
// Richer than UiElementHit when callers need raw match score details.
// ---------------------------------------------------------------------------
struct TemplateMatchResult {
    std::string template_id;
    Rect        bounds;
    float       score      = 0.0f;   // raw match score (0.0 - 1.0)
    float       confidence = 0.0f;   // post-processed confidence
    FrameInfo   frame_info;
};

// ---------------------------------------------------------------------------
// OcrResult
// ---------------------------------------------------------------------------
struct OcrResult {
    std::string text;           // detected text
    std::string normalized;     // lowercased, trimmed
    Rect        bounds;
    float       confidence = 0.0f;
    FrameInfo   frame_info;
};

// ---------------------------------------------------------------------------
// DetectionBundle
// ---------------------------------------------------------------------------
// Aggregate result of a full perception pass on one frame.
// Future-facing: today callers may use the individual result vectors;
// later a single detect(frame) -> DetectionBundle API can be added.
// ---------------------------------------------------------------------------
struct DetectionBundle {
    FrameInfo                      frame;
    std::vector<UiElementHit>      elements;
    std::vector<TemplateMatchResult> template_results;
    std::vector<OcrResult>         ocr_results;
};

} // namespace mirage::perception
