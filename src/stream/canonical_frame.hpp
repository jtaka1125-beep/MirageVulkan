// =============================================================================
// X1 Canonical Frame — immutable decoded frame for AI/macro/OCR
// =============================================================================
#pragma once
#include <cstdint>
#include <memory>
#include "x1_protocol.hpp"  // CANONICAL_W, CANONICAL_H — single source of truth

namespace mirage::x1 {

/**
 * CanonicalFrame
 *
 * The single source of truth for AI, OCR, and macro coordinate calculations.
 *
 * Guarantees:
 *   - width  == CANONICAL_W  (see x1_protocol.hpp)
 *   - height == CANONICAL_H  (see x1_protocol.hpp)
 *   - rgba   == packed RGBA8888, CANONICAL_W * CANONICAL_H * 4 bytes, top-left origin
 *   - No interpolation, no blending, no synthetic content
 *
 * AI/macro consumers MUST use this type exclusively.
 * Never derive coordinate references from PresentationFrame.
 * Never hard-code numeric literals — always use CANONICAL_W / CANONICAL_H.
 */
struct CanonicalFrame {
    uint32_t frame_id  = 0;
    uint64_t pts_us    = 0;
    uint32_t width     = 0;   // always CANONICAL_W
    uint32_t height    = 0;   // always CANONICAL_H
    uint64_t encode_done_us = 0; // Android: encode完了 (ms → us, 32bit MSB of pts_us field)
    uint64_t recv_us   = 0;      // PC-side: UDP受信〜JPEG decode 完了 (monotonic us)

    // RGBA8888, CANONICAL_W * CANONICAL_H * 4 bytes
    std::shared_ptr<uint8_t[]> rgba;

    bool valid() const { return rgba && width > 0 && height > 0; }

    // Convenience pixel accessor (bounds-unchecked)
    const uint8_t* row(int y) const { return rgba.get() + y * width * 4; }
};

} // namespace mirage::x1
