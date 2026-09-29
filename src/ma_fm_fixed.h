// SPDX-License-Identifier: Apache-2.0
#pragma once
//
// ma_fm_fixed.h -- integer-only port of the FM voice engine in ma_fm_core.
// ----------------------------------------------------------------------------
// Same voice model as ma_fm_core (operators, linear-amplitude ADSR, the eight
// connection algorithms, LFO vibrato/tremolo, KSL/KSR), but no float, no double,
// no libm, no heap. Intended for cores without an FPU (RP2040).
//
// Formats:
//   pitch      semitones in Q8, MIDI note 69 (A4, 440 Hz) = 69 * 256
//   phase      32-bit accumulator, 2^32 = one cycle
//   envelope   32-bit level, 0xFFFFFFFF = 1.0
//   gains      Q15 (32768 = 1.0)
//   samples    Q15 (+-32767 = full scale)

#include "ma_fm_core.h"   // FmOpPatch / FmVoicePatch (integer fields only are used)

#include <cstdint>

namespace fxchain::smaf {

// phase increment (2^32 = one cycle per sample) for a pitch in Q8 semitones.
uint64_t fxNoteToIncQ32(int32_t noteQ8, uint32_t sampleRate);

class FxEnvelope {
public:
    void configure(const FmOpPatch& p, uint32_t sampleRate, uint32_t rateScaleQ16);
    void keyOn() { phase_ = Phase::Attack; }
    void keyOff();
    uint32_t advance();
    bool isFinished() const { return phase_ == Phase::Idle; }
private:
    enum class Phase : uint8_t { Idle, Attack, Decay, Sustain, Release };
    Phase phase_ = Phase::Idle;
    bool sustaining_ = true;
    bool xof_ = false;
    uint32_t level_ = 0;
    uint32_t atkStep_ = 0, decStep_ = 0, susStep_ = 0, relStep_ = 0;
    uint32_t susLevel_ = 0;
};

class FxOperator {
public:
    void configure(const FmOpPatch& p, uint32_t sampleRate, uint32_t rateScaleQ16);
    void noteOn(uint64_t baseIncQ32, int32_t noteQ8);
    void noteOff() { env_.keyOff(); }
    void setBase(uint64_t baseIncQ32, int32_t noteQ8);
    // modPhase: phase modulation in 2^32-per-cycle units. lfoQ15: voice LFO.
    int32_t tick(uint32_t modPhase, int32_t lfoQ15);
    bool finished() const { return env_.isFinished(); }
private:
    void recalc_();
    FmOpPatch patch_{};
    FxEnvelope env_{};
    uint64_t baseInc_ = 0;
    int32_t noteQ8_ = 0;
    uint32_t phase_ = 0;
    uint32_t phaseInc_ = 0;
    uint32_t gainQ15_ = 32768;   // total level * key-scale level
    uint32_t vibQ18_ = 0;
    uint32_t amQ15_ = 0;
    bool nyMute_ = false;
};

class FxVoice {
public:
    void setSampleRate(uint32_t sr) { sampleRate_ = sr; }
    void noteOn(const FmVoicePatch& patch, int32_t noteQ8, int32_t velocityQ15);
    void noteOff();
    void setPitch(int32_t noteQ8);
    void setVolume(int32_t volumeQ15) { volumeQ15_ = volumeQ15; updateGain_(); }
    int32_t tick();   // one mono sample, Q15
    bool active() const { return active_; }
private:
    int32_t modOp_(int i, uint32_t extMod);
    void updateGain_();
    FmVoicePatch patch_{};
    FxOperator ops_[4]{};
    uint32_t sampleRate_ = 22050;
    int32_t velocityQ15_ = 32768;
    int32_t volumeQ15_ = 32768;
    int32_t outGainQ15_ = 0;
    bool active_ = false;
    bool fourOp_ = false;
    uint8_t algo_ = 0;
    uint8_t opFb_[4] = {0, 0, 0, 0};
    int32_t fbMem_[4][2] = {{0, 0}, {0, 0}, {0, 0}, {0, 0}};
    uint32_t lfoPhase_ = 0, lfoInc_ = 0;
    int32_t lfoQ15_ = 0;
};

} // namespace fxchain::smaf
