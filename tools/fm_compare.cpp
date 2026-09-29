// SPDX-License-Identifier: Apache-2.0
//
// fm_compare -- renders the same patch and note through the float FM core
// (ma_fm_core) and the integer core (ma_fm_fixed), then reports how far apart
// they are. Bit-exact output is not a goal, so the metrics are phase-insensitive
// (loudness envelope, log spectrum) plus a raw SNR over the attack only.
//
//   fm_compare            summary only
//   fm_compare <outdir>   also writes <outdir>/<case>-float.wav and -fixed.wav

#include "ma_fm_core.h"
#include "ma_fm_fixed.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdio>
#include <string>
#include <vector>

using namespace fxchain::smaf;

namespace {

constexpr uint32_t kRate = 22050;
constexpr int kFrames = kRate * 3 / 2;        // 1.5 s
constexpr int kNoteOffAt = kRate * 3 / 4;     // key-off at 0.75 s

void writeWav(const std::string& path, const std::vector<float>& x) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return;
    uint32_t dataBytes = uint32_t(x.size() * 2), riff = 36 + dataBytes;
    uint32_t sr = kRate, br = kRate * 2, fmtLen = 16;
    uint16_t pcm = 1, ch = 1, align = 2, bits = 16;
    std::fwrite("RIFF", 1, 4, f); std::fwrite(&riff, 4, 1, f);
    std::fwrite("WAVEfmt ", 1, 8, f); std::fwrite(&fmtLen, 4, 1, f);
    std::fwrite(&pcm, 2, 1, f); std::fwrite(&ch, 2, 1, f);
    std::fwrite(&sr, 4, 1, f); std::fwrite(&br, 4, 1, f);
    std::fwrite(&align, 2, 1, f); std::fwrite(&bits, 2, 1, f);
    std::fwrite("data", 1, 4, f); std::fwrite(&dataBytes, 4, 1, f);
    for (float v : x) {
        int s = int(std::lround(std::clamp(v, -1.0f, 1.0f) * 32767.0f));
        int16_t s16 = int16_t(s);
        std::fwrite(&s16, 2, 1, f);
    }
    std::fclose(f);
}

std::vector<float> renderFloat(const FmVoicePatch& p, int note, float vel) {
    FmVoice v;
    v.setSampleRate(kRate);
    v.noteOn(p, 440.0 * std::pow(2.0, (note - 69) / 12.0), vel);
    std::vector<float> out(kFrames);
    for (int i = 0; i < kFrames; ++i) {
        if (i == kNoteOffAt) v.noteOff();
        out[i] = v.tick();
    }
    return out;
}

std::vector<float> renderFixed(const FmVoicePatch& p, int note, float vel) {
    FxVoice v;
    v.setSampleRate(kRate);
    v.noteOn(p, note * 256, int32_t(vel * 32768.0f));
    std::vector<float> out(kFrames);
    for (int i = 0; i < kFrames; ++i) {
        if (i == kNoteOffAt) v.noteOff();
        out[i] = float(v.tick()) / 32768.0f;
    }
    return out;
}

double frameDb(const std::vector<float>& x, int start, int n) {
    double e = 0;
    for (int i = start; i < start + n && i < int(x.size()); ++i) e += double(x[i]) * x[i];
    e /= n;
    return e > 1e-12 ? 10.0 * std::log10(e) : -120.0;
}

void fft(std::vector<std::complex<double>>& a) {
    const size_t n = a.size();
    for (size_t i = 1, j = 0; i < n; ++i) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (size_t len = 2; len <= n; len <<= 1) {
        double ang = -2 * M_PI / double(len);
        std::complex<double> wl(std::cos(ang), std::sin(ang));
        for (size_t i = 0; i < n; i += len) {
            std::complex<double> w(1);
            for (size_t j = 0; j < len / 2; ++j) {
                auto u = a[i + j], v = a[i + j + len / 2] * w;
                a[i + j] = u + v;
                a[i + j + len / 2] = u - v;
                w *= wl;
            }
        }
    }
}

std::vector<double> spectrumDb(const std::vector<float>& x, int start) {
    const int N = 1024;
    std::vector<std::complex<double>> a(N);
    for (int i = 0; i < N; ++i) {
        double w = 0.5 - 0.5 * std::cos(2 * M_PI * i / N);
        a[i] = (start + i < int(x.size()) ? x[start + i] : 0.0f) * w;
    }
    fft(a);
    std::vector<double> db(N / 2);
    for (int k = 0; k < N / 2; ++k) {
        double m = std::abs(a[k]) / (N / 4.0);
        db[k] = std::max(20.0 * std::log10(m + 1e-9), -90.0);
    }
    return db;
}

struct Metrics {
    double envMaxDb = 0, envMeanDb = 0, lsdDb = 0, attackSnrDb = 0, peakRatioDb = 0;
};

Metrics compare(const std::vector<float>& ref, const std::vector<float>& fx) {
    Metrics m;
    // loudness envelope in 10 ms frames, only where either side is above -60 dB
    const int fn = kRate / 100;
    int cnt = 0;
    for (int s = 0; s + fn <= kFrames; s += fn) {
        double a = frameDb(ref, s, fn), b = frameDb(fx, s, fn);
        if (a < -60.0 && b < -60.0) continue;
        double d = std::fabs(a - b);
        m.envMaxDb = std::max(m.envMaxDb, d);
        m.envMeanDb += d;
        ++cnt;
    }
    if (cnt) m.envMeanDb /= cnt;
    // log-spectral distance, 1024-point frames, bins with energy only
    int frames = 0;
    for (int s = 0; s + 1024 <= kNoteOffAt; s += 1024) {
        auto A = spectrumDb(ref, s), B = spectrumDb(fx, s);
        double acc = 0; int nb = 0;
        for (size_t k = 1; k < A.size(); ++k) {
            if (A[k] < -70.0 && B[k] < -70.0) continue;
            acc += (A[k] - B[k]) * (A[k] - B[k]); ++nb;
        }
        if (nb) { m.lsdDb += std::sqrt(acc / nb); ++frames; }
    }
    if (frames) m.lsdDb /= frames;
    // raw waveform SNR over the first 2048 samples (attack; phase drift still small)
    double sig = 0, err = 0;
    for (int i = 0; i < 2048; ++i) { sig += double(ref[i]) * ref[i]; double d = ref[i] - fx[i]; err += d * d; }
    m.attackSnrDb = err > 0 ? 10.0 * std::log10((sig + 1e-12) / err) : 99.0;
    float pr = 0, pf = 0;
    for (int i = 0; i < kFrames; ++i) { pr = std::max(pr, std::fabs(ref[i])); pf = std::max(pf, std::fabs(fx[i])); }
    m.peakRatioDb = (pr > 1e-6f && pf > 1e-6f) ? 20.0 * std::log10(double(pf) / pr) : 0.0;
    return m;
}

// pitch table accuracy: fixed phase increment against the exact value, in cents
void pitchTest() {
    double worst = 0;
    int worstNote = 0;
    for (int q8 = 0; q8 <= 127 * 256; q8 += 7) {
        double hz = 440.0 * std::pow(2.0, (q8 - 69 * 256) / 3072.0);
        if (hz >= kRate / 2.0) break;
        double exact = hz / kRate * 4294967296.0;
        double got = double(fxNoteToIncQ32(q8, kRate));
        double cents = std::fabs(1200.0 * std::log2(got / exact));
        if (cents > worst) { worst = cents; worstNote = q8; }
    }
    std::printf("pitch table: worst error %.3f cents (at note %.2f), below Nyquist\n",
                worst, worstNote / 256.0);
}

FmVoicePatch fourOpPatch(int algo, int wave0, int wave1) {
    FmVoicePatch v = FmVoicePatch::defaultPatch();
    v.fourOp = true;
    v.algorithm = uint8_t(algo);
    v.feedback = 3;
    v.lfo = 2;
    v.ops[0] = FmOpPatch{2, 14, 15, 5, 2, 7, 4, 1, 1, uint8_t(wave0), 3, 2, true, true, true};
    v.ops[1] = FmOpPatch{1, 0, 13, 4, 1, 6, 3, 0, 0, uint8_t(wave1), 5, 0, false, true, true};
    v.ops[2] = FmOpPatch{3, 18, 15, 6, 2, 8, 5, 2, 0, 0, 2, 0, true, false, true};
    v.ops[3] = FmOpPatch{1, 2, 14, 3, 1, 5, 2, 0, 1, 0, 4, 0, false, false, true};
    v.ops[0].dvb = 2; v.ops[1].dvb = 1; v.ops[0].dam = 1; v.ops[2].dam = 2;
    return v;
}

struct Case { std::string name; FmVoicePatch patch; int note; };

} // namespace

int main(int argc, char** argv) {
    std::string outDir = argc > 1 ? argv[1] : "";
    pitchTest();

    std::vector<Case> cases;
    cases.push_back({"default-c4", FmVoicePatch::defaultPatch(), 60});
    for (int prog : {0, 8, 16, 24, 32, 40, 48, 56, 64, 72, 80, 88, 96, 104, 112, 120})
        cases.push_back({"gm" + std::to_string(prog) + "-c4", FmVoicePatch::gmApprox(prog), 60});
    for (int note : {36, 72, 84})
        cases.push_back({"gm0-n" + std::to_string(note), FmVoicePatch::gmApprox(0), note});
    for (int note : {36, 48, 60, 72})
        cases.push_back({"drum-n" + std::to_string(note), FmVoicePatch::drumApprox(note), note});
    for (int algo = 0; algo < 8; ++algo) {
        FmVoicePatch p = fourOpPatch(algo, algo & 7, (algo + 3) & 7);
        if (algo < 2) p.fourOp = false;
        cases.push_back({"fourop-algo" + std::to_string(algo), p, 64});
    }
    for (int w = 0; w < 8; ++w) {
        FmVoicePatch p = FmVoicePatch::defaultPatch();
        p.ops[0].wave = uint8_t(w); p.ops[1].wave = uint8_t(w);
        cases.push_back({"wave" + std::to_string(w), p, 57});
    }

    std::printf("\n%-16s %8s %8s %8s %10s %8s\n", "case", "envMax", "envMean", "LSD", "attackSNR", "peakRatio");
    std::printf("%-16s %8s %8s %8s %10s %8s\n", "", "[dB]", "[dB]", "[dB]", "[dB]", "[dB]");
    Metrics worst{}, sum{};
    for (const Case& c : cases) {
        auto a = renderFloat(c.patch, c.note, 0.9f);
        auto b = renderFixed(c.patch, c.note, 0.9f);
        Metrics m = compare(a, b);
        std::printf("%-16s %8.2f %8.2f %8.2f %10.1f %8.2f\n", c.name.c_str(),
                    m.envMaxDb, m.envMeanDb, m.lsdDb, m.attackSnrDb, m.peakRatioDb);
        worst.envMaxDb = std::max(worst.envMaxDb, m.envMaxDb);
        worst.lsdDb = std::max(worst.lsdDb, m.lsdDb);
        worst.peakRatioDb = std::max(worst.peakRatioDb, std::fabs(m.peakRatioDb));
        sum.envMeanDb += m.envMeanDb; sum.lsdDb += m.lsdDb;
        if (!outDir.empty()) {
            writeWav(outDir + "/" + c.name + "-float.wav", a);
            writeWav(outDir + "/" + c.name + "-fixed.wav", b);
        }
    }
    std::printf("\n%zu cases: worst envMax %.2f dB, mean envMean %.2f dB, worst LSD %.2f dB, mean LSD %.2f dB, worst |peakRatio| %.2f dB\n",
                cases.size(), worst.envMaxDb, sum.envMeanDb / cases.size(), worst.lsdDb,
                sum.lsdDb / cases.size(), worst.peakRatioDb);
    return 0;
}
