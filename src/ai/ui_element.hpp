#pragma once
// =============================================================================
// UiElementHit - Layer1->Layer2 UI element structure
// =============================================================================
#include <string>
#include <vector>
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace mirage::ai {

enum class UiElementSource { TemplateNcc, Ocr, IconMatch, Unknown };

struct Rect { int x = 0, y = 0, w = 0, h = 0; };

// Text normalization: lowercase, trim, fullwidth->halfwidth, collapse spaces
std::string normalizeText(const std::string& s);

float calcIoU(const Rect& a, const Rect& b);
float centerDist(const Rect& a, const Rect& b);
float textSimilarity(const std::string& a, const std::string& b);

struct UiElementHit {
    std::string     type;             // "button" / "text" / "icon"
    std::string     text;             // raw OCR or template name
    std::string     text_normalized;  // normalizeText(text)

    Rect  bbox;
    float cx_norm = 0.f;   // bbox center / screen_w
    float cy_norm = 0.f;   // bbox center / screen_h

    float           confidence = 0.f;
    UiElementSource source     = UiElementSource::Unknown;
    float           raw_score  = 0.f;

    bool merged = false;   // set true when absorbed by dedup

    static UiElementHit make(const std::string& type, const std::string& text,
                              Rect bbox, float confidence, UiElementSource source,
                              float raw_score, int screen_w = 1, int screen_h = 1);
};

bool isSameElement(const UiElementHit& a, const UiElementHit& b,
                   float iou_threshold  = 0.4f,
                   float center_px_thr  = 30.f,
                   float text_sim_thr   = 0.8f);

std::vector<UiElementHit> deduplicateElements(
    std::vector<UiElementHit> hits,
    float iou_threshold = 0.4f,
    float center_px_thr = 30.f,
    float text_sim_thr  = 0.8f);

} // namespace mirage::ai
