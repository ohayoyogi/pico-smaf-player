// SPDX-License-Identifier: Apache-2.0
//
// ma_fm_fixed.cpp -- integer-only FM voice engine. See ma_fm_fixed.h.
// All lookup tables come from tools/gen_fm_tables.py; nothing here calls libm.

#include "ma_fm_fixed.h"

namespace fxchain::smaf {

namespace {

#include "ma_fm_tables.inc"

constexpr uint32_t kOne32 = 0xFFFFFFFFu;

struct Pow2 { uint32_t m; int oct; };   // value = (m / 65536) * 2^oct, m in [65536, 131072)

// 2^(e / 65536) split into mantissa and octave.
inline Pow2 pow2Split(int32_t eQ16) {
    Pow2 r;
    r.oct = eQ16 >> 16;
    uint32_t f = uint32_t(eQ16) & 0xFFFFu;
    uint32_t idx = f >> 10, fr = f & 1023u;
    uint32_t a = kPow2Frac[idx], b = kPow2Frac[idx + 1];
    r.m = a + (((b - a) * fr) >> 10);
    return r;
}

// 2^(e / 65536) as Q16, for small exponents (gains, rate scales).
inline uint32_t pow2Q16(int32_t eQ16) {
    Pow2 p = pow2Split(eQ16);
    if (p.oct >= 0) return p.oct > 12 ? (p.m << 12) : (p.m << p.oct);
    return p.oct < -31 ? 0u : (p.m >> -p.oct);
}

inline int32_t abs32(int32_t v) { return v < 0 ? -v : v; }

// waveform family (OPL set), idx = phase >> 22 (0..1023), result Q15.
inline int32_t waveFx(uint8_t wave, uint32_t idx) {
    switch (wave & 7) {
        case 0: return kSin[idx];
        case 1: { int32_t s = kSin[idx]; return s > 0 ? s : 0; }
        case 2: return abs32(kSin[idx]);
        case 3: { uint32_t q = idx & 511u; return q < 256u ? kSin[q] : 0; }
        case 4: return idx < 512u ? kSin[(idx * 2u) & 1023u] : 0;
        case 5: return idx < 512u ? abs32(kSin[(idx * 2u) & 1023u]) : 0;
        case 6: return kSqr[idx];
        default: return kSaw[idx];
    }
}

// chip "rate" 0..15 -> per-sample step of a 0..1 (Q32) level, before KSR scaling.
uint32_t rateToStep(uint8_t rate, uint32_t sr, bool attack) {
    if (rate == 0) {
        if (attack) return 0;
        return uint32_t((uint64_t(1) << 32) / (uint64_t(sr) * 20u));
    }
    uint64_t us = attack ? kAtkUs[rate & 15] : kDecUs[rate & 15];
    uint64_t samples = us * sr / 1000000u;
    if (samples < 1) samples = 1;
    uint64_t step = (uint64_t(1) << 32) / samples;
    return step > kOne32 ? kOne32 : uint32_t(step);
}

inline uint32_t scaleStep(uint32_t step, uint32_t rsQ16) {
    uint64_t v = (uint64_t(step) * rsQ16) >> 16;
    return v > kOne32 ? kOne32 : uint32_t(v);
}

constexpr int32_t kModDepthQ32PerQ15 = 55050;  // 0.42 cycles * 2^32 / 2^15

// modular 32-bit multiply: equals the exact product mod 2^32, which is all a phase needs.
inline uint32_t modPhaseOf(int32_t sumQ15) {
    return uint32_t(sumQ15) * uint32_t(kModDepthQ32PerQ15);
}

const uint8_t kLfoHz10[4] = { 18, 40, 60, 97 };   // 1.8 / 4.0 / 6.0 / 9.7 Hz
const uint16_t kVibQ18[4] = { 514, 1015, 2029, 4058 };         // 0.00196 ... 0.01548
const uint16_t kTremQ15[4] = { 4227, 7930, 13893, 21922 };     // 0.129 ... 0.669
} // namespace

uint64_t fxNoteToIncQ32(int32_t noteQ8, uint32_t sr) {
    int32_t x = noteQ8 - 69 * 256;
    int32_t e = int32_t((int64_t(x) * 65536) / 3072);   // octaves, Q16
    Pow2 p = pow2Split(e);
    uint64_t base = ((uint64_t(440) * p.m) << 16) / sr;
    if (p.oct >= 0) return p.oct > 20 ? (base << 20) : (base << p.oct);
    return p.oct < -40 ? 0u : (base >> -p.oct);
}

// ── FxEnvelope ───────────────────────────────────────────────────────────────
void FxEnvelope::configure(const FmOpPatch& p, uint32_t sr, uint32_t rs) {
    atkStep_ = scaleStep(rateToStep(p.ar, sr, true), rs);
    decStep_ = scaleStep(rateToStep(p.dr, sr, false), rs);
    susStep_ = scaleStep(rateToStep(p.sr, sr, false), rs);
    relStep_ = scaleStep(rateToStep(p.rr, sr, false), rs);
    susLevel_ = uint32_t(15 - (p.sl & 15)) * 286331153u;   // (15 - sl) / 15
    sustaining_ = p.egType;
    xof_ = p.xof;
    phase_ = Phase::Idle;
    level_ = 0;
}

void FxEnvelope::keyOff() {
    if (xof_ && !sustaining_) return;
    if (phase_ != Phase::Idle) phase_ = Phase::Release;
}

uint32_t FxEnvelope::advance() {
    switch (phase_) {
        case Phase::Idle: return 0;
        case Phase::Attack: {
            uint32_t step = atkStep_ ? atkStep_ : kOne32;
            if (level_ >= kOne32 - step) { level_ = kOne32; phase_ = Phase::Decay; }
            else level_ += step;
            break;
        }
        case Phase::Decay:
            if (level_ <= decStep_ || level_ - decStep_ <= susLevel_) {
                level_ = susLevel_;
                phase_ = sustaining_ ? Phase::Sustain : Phase::Release;
            } else {
                level_ -= decStep_;
            }
            break;
        case Phase::Sustain:
            if (level_ <= susStep_) { level_ = 0; phase_ = Phase::Idle; }
            else level_ -= susStep_;
            break;
        case Phase::Release:
            if (level_ <= relStep_) { level_ = 0; phase_ = Phase::Idle; }
            else level_ -= relStep_;
            break;
    }
    return level_;
}

// ── FxOperator ───────────────────────────────────────────────────────────────
void FxOperator::configure(const FmOpPatch& p, uint32_t sr, uint32_t rs) {
    patch_ = p;
    vibQ18_ = p.vib ? kVibQ18[p.dvb & 3] : 0;
    amQ15_ = p.am ? kTremQ15[p.dam & 3] : 0;
    env_.configure(p, sr, rs);
}

void FxOperator::recalc_() {
    uint64_t inc = (patch_.multi == 0) ? (baseInc_ >> 1) : baseInc_ * (patch_.multi & 15u);
    int32_t detQ16 = 65536 + (((int32_t(patch_.dt) * 2 - 7) * 315) >> 4);   // 1 + (dt - 3.5) * 0.0006
    inc = (inc * uint32_t(detQ16)) >> 16;
    nyMute_ = inc >= 0x80000000ull;
    phaseInc_ = nyMute_ ? 0u : uint32_t(inc);
    // key-scale level: attenuation grows with octaves above middle C.
    int32_t above = noteQ8_ - 60 * 256;
    if (above < 0) above = 0;
    int32_t octQ16 = int32_t((int64_t(above) * 65536) / 3072);
    int32_t attQ16 = int32_t((int64_t(octQ16) * kKslExpQ16[patch_.ksl & 3]) >> 16);
    uint32_t kslQ15 = pow2Q16(-attQ16) >> 1;
    gainQ15_ = (uint32_t(kTlGain[patch_.tl < 63 ? patch_.tl : 63]) * kslQ15) >> 15;
}

void FxOperator::noteOn(uint64_t baseInc, int32_t noteQ8) {
    baseInc_ = baseInc;
    noteQ8_ = noteQ8;
    phase_ = 0;
    recalc_();
    env_.keyOn();
}

void FxOperator::setBase(uint64_t baseInc, int32_t noteQ8) {
    baseInc_ = baseInc;
    noteQ8_ = noteQ8;
    recalc_();
}

int32_t FxOperator::tick(uint32_t modPhase, int32_t lfoQ15) {
    int32_t env = int32_t(env_.advance() >> 17);   // Q15
    if (nyMute_) return 0;
    uint32_t inc = phaseInc_;
    if (vibQ18_) {
        // inc < 2^31 and |factor| <= 4058 keep the product inside int32.
        int32_t factor = (int32_t(vibQ18_) * lfoQ15) >> 15;
        inc = uint32_t(int32_t(inc) + (((int32_t(inc >> 12)) * factor) >> 6));
    }
    phase_ += inc;
    int32_t w = waveFx(patch_.wave, (phase_ + modPhase) >> 22);
    int32_t o = (w * env) >> 15;
    o = (o * int32_t(gainQ15_)) >> 15;
    if (amQ15_) {
        int32_t dip = (int32_t(amQ15_) * (16384 + (lfoQ15 >> 1))) >> 15;
        o = (o * (32768 - dip)) >> 15;
    }
    return o;
}

// ── FxVoice ──────────────────────────────────────────────────────────────────
void FxVoice::updateGain_() {
    int32_t g = (velocityQ15_ * volumeQ15_) >> 15;
    outGainQ15_ = (g * 22938) >> 15;   // 0.7
}

void FxVoice::noteOn(const FmVoicePatch& patch, int32_t noteQ8, int32_t velocityQ15) {
    patch_ = patch;
    fourOp_ = patch.fourOp;
    algo_ = patch.algorithm & 7;
    velocityQ15_ = velocityQ15 < 0 ? 0 : (velocityQ15 > 32768 ? 32768 : velocityQ15);
    updateGain_();
    for (int i = 0; i < 4; ++i) {
        fbMem_[i][0] = fbMem_[i][1] = 0;
        opFb_[i] = patch.ops[i].fb;
    }
    if (patch.feedback && !opFb_[0]) opFb_[0] = patch.feedback;
    lfoInc_ = uint32_t((uint64_t(kLfoHz10[patch.lfo & 3]) << 32) / (10u * sampleRate_));
    lfoPhase_ = 0;
    lfoQ15_ = 0;
    // KSR: envelopes speed up as the note rises, 2^(semis/24) or 2^(semis/96).
    int32_t semisUpQ8 = noteQ8 - 60 * 256;
    uint64_t base = fxNoteToIncQ32(noteQ8, sampleRate_);
    int nops = fourOp_ ? 4 : 2;
    for (int i = 0; i < nops; ++i) {
        int32_t d = patch.ops[i].ksr ? 24 : 96;
        int32_t eQ16 = int32_t((int64_t(semisUpQ8) * 256) / d);
        uint32_t rs = pow2Q16(eQ16);
        if (rs < 32768u) rs = 32768u;
        if (rs > 262144u) rs = 262144u;
        ops_[i].configure(patch.ops[i], sampleRate_, rs);
        ops_[i].noteOn(base, noteQ8);
    }
    active_ = true;
}

void FxVoice::noteOff() {
    int nops = fourOp_ ? 4 : 2;
    for (int i = 0; i < nops; ++i) ops_[i].noteOff();
}

void FxVoice::setPitch(int32_t noteQ8) {
    uint64_t base = fxNoteToIncQ32(noteQ8, sampleRate_);
    int nops = fourOp_ ? 4 : 2;
    for (int i = 0; i < nops; ++i) ops_[i].setBase(base, noteQ8);
}

int32_t FxVoice::modOp_(int i, uint32_t extMod) {
    uint32_t fbc = 0;
    if (opFb_[i]) {
        int32_t avg = (fbMem_[i][0] + fbMem_[i][1]) >> 1;
        fbc = uint32_t(avg * int32_t(opFb_[i]) * 5461);
    }
    int32_t o = ops_[i].tick(extMod + fbc, lfoQ15_);
    fbMem_[i][1] = fbMem_[i][0];
    fbMem_[i][0] = o;
    return o;
}

int32_t FxVoice::tick() {
    if (!active_) return 0;
    lfoPhase_ += lfoInc_;
    lfoQ15_ = kSin[lfoPhase_ >> 22];
    int32_t out = 0;
    switch (algo_) {
        case 0: {
            int32_t m = modOp_(0, 0);
            out = modOp_(1, modPhaseOf(m));
        } break;
        case 1: {
            int32_t a = modOp_(0, 0), b = modOp_(1, 0);
            out = (a + b) >> 1;
        } break;
        case 2: {
            int32_t a = modOp_(0, 0), b = modOp_(1, 0), c = modOp_(2, 0), e = modOp_(3, 0);
            out = ((a + b + c + e) * 11469) >> 15;   // 0.35
        } break;
        case 3: {
            int32_t a = modOp_(0, 0), b = modOp_(1, 0);
            int32_t c = modOp_(2, modPhaseOf(a + b));
            out = modOp_(3, modPhaseOf(c));
        } break;
        case 4: {
            int32_t a = modOp_(0, 0);
            int32_t b = modOp_(1, modPhaseOf(a));
            int32_t c = modOp_(2, modPhaseOf(b));
            out = modOp_(3, modPhaseOf(c));
        } break;
        case 5: {
            int32_t a = modOp_(0, 0);
            int32_t b = modOp_(1, modPhaseOf(a));
            int32_t c = modOp_(2, 0);
            int32_t e = modOp_(3, modPhaseOf(c));
            out = (b + e) >> 1;
        } break;
        case 6: {
            int32_t a = modOp_(0, 0), b = modOp_(1, 0);
            int32_t c = modOp_(2, modPhaseOf(b));
            int32_t e = modOp_(3, modPhaseOf(c));
            out = (a + e) >> 1;
        } break;
        default: {
            int32_t a = modOp_(0, 0), b = modOp_(1, 0);
            int32_t c = modOp_(2, modPhaseOf(b));
            int32_t e = modOp_(3, 0);
            out = ((a + c + e) * 13107) >> 15;   // 0.4
        } break;
    }
    bool anyLive = false;
    int nops = fourOp_ ? 4 : 2;
    for (int i = 0; i < nops; ++i) if (!ops_[i].finished()) { anyLive = true; break; }
    if (!anyLive) active_ = false;
    return (out * outGainQ15_) >> 15;
}

} // namespace fxchain::smaf
