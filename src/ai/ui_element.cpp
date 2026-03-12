#include "ai/ui_element.hpp"
#include <cctype>

namespace mirage::ai {

std::string normalizeText(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    bool prev_sp = true;
    for (size_t i = 0; i < s.size(); ) {
        unsigned char c0 = (unsigned char)s[i];
        uint32_t cp = 0;
        int bc = 1;
        if      (c0 < 0x80) { cp = c0; bc = 1; }
        else if (c0 < 0xE0) { cp = ((c0&0x1F)<<6)  | ((unsigned char)s[i+1]&0x3F); bc = 2; }
        else if (c0 < 0xF0) { cp = ((c0&0x0F)<<12) | (((unsigned char)s[i+1]&0x3F)<<6)  | ((unsigned char)s[i+2]&0x3F); bc = 3; }
        else                 { cp = ((c0&0x07)<<18) | (((unsigned char)s[i+1]&0x3F)<<12) | (((unsigned char)s[i+2]&0x3F)<<6) | ((unsigned char)s[i+3]&0x3F); bc = 4; }
        i += bc;

        // fullwidth alphanumeric (U+FF01-FF5E) -> halfwidth
        if (cp >= 0xFF01 && cp <= 0xFF5E) cp -= 0xFEE0;
        // fullwidth space
        if (cp == 0x3000) cp = 0x0020;
        // collapse spaces
        if (cp == 0x0020 || cp == 0x0009) {
            if (!prev_sp) { out += ' '; prev_sp = true; }
            continue;
        }
        prev_sp = false;
        // lowercase ASCII
        if (cp >= 'A' && cp <= 'Z') cp += 32;
        // encode back to UTF-8
        if      (cp < 0x80)    { out += (char)cp; }
        else if (cp < 0x800)   { out += (char)(0xC0|(cp>>6)); out += (char)(0x80|(cp&0x3F)); }
        else if (cp < 0x10000) { out += (char)(0xE0|(cp>>12)); out += (char)(0x80|((cp>>6)&0x3F)); out += (char)(0x80|(cp&0x3F)); }
        else                   { out += (char)(0xF0|(cp>>18)); out += (char)(0x80|((cp>>12)&0x3F)); out += (char)(0x80|((cp>>6)&0x3F)); out += (char)(0x80|(cp&0x3F)); }
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

float calcIoU(const Rect& a, const Rect& b) {
    int ix  = std::max(a.x, b.x),       iy  = std::max(a.y, b.y);
    int ix2 = std::min(a.x+a.w, b.x+b.w), iy2 = std::min(a.y+a.h, b.y+b.h);
    if (ix2 <= ix || iy2 <= iy) return 0.f;
    float inter = (float)(ix2-ix)*(iy2-iy);
    return inter / ((float)a.w*a.h + (float)b.w*b.h - inter);
}

float centerDist(const Rect& a, const Rect& b) {
    float dx = (a.x + a.w*0.5f) - (b.x + b.w*0.5f);
    float dy = (a.y + a.h*0.5f) - (b.y + b.h*0.5f);
    return std::sqrt(dx*dx + dy*dy);
}

float textSimilarity(const std::string& a, const std::string& b) {
    if (a.empty() && b.empty()) return 1.f;
    if (a.empty() || b.empty()) return 0.f;
    if (a == b) return 1.f;
    int c = 0;
    int lim = (int)std::min(a.size(), b.size());
    for (int k = 0; k < lim; ++k) {
        if (a[k] == b[k]) ++c; else break;
    }
    return (float)c / (float)std::max(a.size(), b.size());
}

UiElementHit UiElementHit::make(const std::string& type, const std::string& text,
                                  Rect bbox, float confidence, UiElementSource source,
                                  float raw_score, int sw, int sh) {
    UiElementHit h;
    h.type            = type;
    h.text            = text;
    h.text_normalized = normalizeText(text);
    h.bbox            = bbox;
    h.cx_norm         = (bbox.x + bbox.w * 0.5f) / (float)sw;
    h.cy_norm         = (bbox.y + bbox.h * 0.5f) / (float)sh;
    h.confidence      = confidence;
    h.source          = source;
    h.raw_score       = raw_score;
    return h;
}

bool isSameElement(const UiElementHit& a, const UiElementHit& b,
                   float iou_thr, float cp_thr, float ts_thr) {
    bool spatial  = (calcIoU(a.bbox, b.bbox) > iou_thr) ||
                    (centerDist(a.bbox, b.bbox) < cp_thr);
    bool semantic = (a.type == b.type) &&
                    (textSimilarity(a.text_normalized, b.text_normalized) > ts_thr);
    return spatial && semantic;
}

std::vector<UiElementHit> deduplicateElements(std::vector<UiElementHit> hits,
    float iou_thr, float cp_thr, float ts_thr) {
    for (int i = 0; i < (int)hits.size(); ++i) {
        if (hits[i].merged) continue;
        for (int j = i+1; j < (int)hits.size(); ++j) {
            if (hits[j].merged) continue;
            if (isSameElement(hits[i], hits[j], iou_thr, cp_thr, ts_thr)) {
                if (hits[j].confidence > hits[i].confidence)
                    std::swap(hits[i], hits[j]);
                hits[j].merged = true;
            }
        }
    }
    hits.erase(std::remove_if(hits.begin(), hits.end(),
        [](const UiElementHit& h){ return h.merged; }), hits.end());
    return hits;
}

} // namespace mirage::ai
