// SPDX-License-Identifier: Apache-2.0
#pragma once
//
// yamaha_adpcm.h -- the 4-bit yamaha adpcm codec used by smaf ATR / stream-pcm.
// ----------------------------------------------------------------------------
// this is the YM2608 (opna) adpcm-b family, the same 4-bit codec yamaha reused
// across their fm parts. one nibble in, one 16-bit sample out, with an adaptive
// step size. the algorithm below is the published open form (delta built from
// the step fraction, then a small step-adaptation table). it decodes to plain
// int16 pcm; the mixer resamples it to the device rate.
//
// nibble order is the one wrinkle: MA files pack two codes per byte and the two
// references disagree on which nibble comes first. we default to high-nibble-
// first (standard for the yamaha chips) and expose the flag so the mixer can
// flip it if a specific file decodes to hash.

#include <cstdint>
#include <vector>
#include <algorithm>

namespace fxchain::smaf {

class YamahaAdpcm {
public:
    void reset() { last_ = 0; step_ = 127; }

    // decode one 4-bit code to a 16-bit pcm sample.
    int16_t decodeNibble(uint8_t code) {
        int delta = step_ >> 3;
        if (code & 1) delta += step_ >> 2;
        if (code & 2) delta += step_ >> 1;
        if (code & 4) delta += step_;
        if (code & 8) delta = -delta;
        last_ = clamp16(last_ + delta);
        // step adaptation (opna-b table, expressed as the published ratios)
        switch (code & 7) {
            case 0: case 1: case 2: case 3: step_ = step_ * 115 / 128; break;
            case 4: step_ = step_ * 307 / 256; break;
            case 5: step_ = step_ * 409 / 256; break;
            case 6: step_ = step_ * 2;          break;
            default: step_ = step_ * 307 / 128; break;   // 7
        }
        step_ = std::clamp(step_, 127, 24576);
        return int16_t(last_);
    }

    // decode a whole packed buffer to int16 pcm. highNibbleFirst chooses which
    // 4 bits of each byte are decoded first.
    std::vector<int16_t> decodeAll(const uint8_t* data, size_t n, bool highNibbleFirst = true) {
        reset();
        std::vector<int16_t> out;
        out.reserve(n * 2);
        for (size_t i = 0; i < n; ++i) {
            uint8_t b = data[i];
            uint8_t hi = (b >> 4) & 0x0f, lo = b & 0x0f;
            if (highNibbleFirst) { out.push_back(decodeNibble(hi)); out.push_back(decodeNibble(lo)); }
            else                 { out.push_back(decodeNibble(lo)); out.push_back(decodeNibble(hi)); }
        }
        return out;
    }

private:
    static int clamp16(int v) { return v < -32768 ? -32768 : (v > 32767 ? 32767 : v); }
    int last_ = 0;
    int step_ = 127;
};

} // namespace fxchain::smaf
