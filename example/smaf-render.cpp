// SPDX-License-Identifier: Apache-2.0
//
// smaf-render.cpp -- decode a yamaha smaf (.mmf) ringtone to a wav file.
// ----------------------------------------------------------------------------
// the whole point of the library in one file: read a .mmf, parse it, synth it,
// write a 16-bit stereo wav you can play anywhere. no qt, no deps, under 100 lines.
//
//   smaf-render tune.mmf out.wav [seconds]
//
// build it with the CMakeLists in the repo root, or by hand from the repo root:
//   c++ -std=c++20 -Isrc example/smaf-render.cpp src/*.cpp -o smaf-render

#include "smaf_file.h"
#include "ma_player.h"

#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <vector>
#include <fstream>
#include <string>

using namespace fxchain::smaf;

static std::vector<uint8_t> readAll(const char* path) {
    std::ifstream f(path, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)),
                                 std::istreambuf_iterator<char>());
}

static void writeWav(const char* path, const std::vector<float>& s, uint32_t rate) {
    std::ofstream f(path, std::ios::binary);
    uint32_t nb = uint32_t(s.size()) * 2;               // 2 bytes per sample
    uint32_t chunk = 36 + nb, byteRate = rate * 2 * 2;
    auto w32 = [&](uint32_t v) { f.write(reinterpret_cast<char*>(&v), 4); };
    auto w16 = [&](uint16_t v) { f.write(reinterpret_cast<char*>(&v), 2); };
    f.write("RIFF", 4); w32(chunk); f.write("WAVE", 4);
    f.write("fmt ", 4); w32(16); w16(1); w16(2); w32(rate); w32(byteRate); w16(4); w16(16);
    f.write("data", 4); w32(nb);
    for (float v : s) {
        int i = int(v * 32767.0f);
        i = i > 32767 ? 32767 : (i < -32768 ? -32768 : i);
        int16_t o = int16_t(i); f.write(reinterpret_cast<char*>(&o), 2);
    }
}

int main(int argc, char** argv) {
    if (argc < 3) {
        std::printf("usage: smaf-render <tune.mmf> <out.wav> [seconds]\n");
        return 2;
    }
    double seconds = (argc >= 4) ? std::atof(argv[3]) : 30.0;
    const uint32_t rate = 44100;

    auto bytes = readAll(argv[1]);
    if (bytes.size() < 12) { std::printf("cannot read %s\n", argv[1]); return 1; }

    SmafFile file;
    if (!file.parse(bytes.data(), bytes.size())) {
        std::printf("not a valid SMAF file: %s\n", argv[1]);
        return 1;
    }
    std::printf("SMAF: title='%s' vendor='%s' tracks=%zu\n",
                file.title.c_str(), file.vendor.c_str(), file.tracks.size());

    MaPlayer player;
    if (!player.init(file, rate)) { std::printf("no playable score track\n"); return 1; }

    std::vector<float> out;
    int total = int(seconds * rate);
    float buf[1024 * 2];
    int done = 0; double peak = 0;
    while (done < total) {
        int want = (total - done < 1024) ? (total - done) : 1024;
        int got = player.render(buf, want);
        if (got <= 0) break;
        for (int i = 0; i < got * 2; ++i) {
            out.push_back(buf[i]);
            float a = buf[i] < 0 ? -buf[i] : buf[i];
            if (a > peak) peak = a;
        }
        done += got;
    }
    writeWav(argv[2], out, rate);
    std::printf("wrote %s (%d frames, peak %.3f)\n", argv[2], done, peak);
    return peak > 0.001 ? 0 : 3;   // exit 3 = parsed + played, but silent
}
