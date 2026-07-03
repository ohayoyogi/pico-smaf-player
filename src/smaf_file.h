// SPDX-License-Identifier: Apache-2.0
#pragma once
//
// smaf_file.h -- yamaha smaf (.mmf) container parser. the file format of the
//                polyphonic-ringtone era.
// ----------------------------------------------------------------------------
// smaf is what yamaha's MA-series phone chips (MA-1/2/3/5, the YMU762 family)
// ate for breakfast between 1999 and ~2008. every "polyphonic ringtone" you
// heard on a samsung/lg/sharp/panasonic of that era was one of these files.
//
// the container is dead simple and pleasantly honest: big-endian TLV chunks,
// 4-byte ascii id + 4-byte size, the whole file wrapped in one 'MMMD' chunk
// and closed with a CRC-16 over everything before it. the 4th character of a
// track chunk id is NOT ascii, it is the track number ('MTR\x05' = score
// track 5). two generations of score track live inside:
//
//   format 0x00  "HandyPhone Standard"  (MA-1/MA-2)  4 voices, 2-byte events
//   format 0x01  "Mobile Standard, huffman-compressed" (MA-3/5)
//   format 0x02  "Mobile Standard, uncompressed"       (MA-3/5) 16-ch,
//                midi-flavoured status bytes + variable-length delta times
//
// this header is the QT-FREE half of the smaf stack: pure parsing, no
// synthesis, no allocation tricks, everything lands in plain std::vector so
// the sequencer + fm core can be unit-tested against it in isolation.
//
// spec sources (read as documentation, no code copied): the public SMAF
// specification chapters yamaha used to host, umjammer/vavi-sound (java,
// behavioural reference), and hex ground-truth over a 500-file corpus of
// real handset ringtones. every offset below was verified against real
// bytes before it was trusted.

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>

namespace fxchain::smaf {

// ── wave payloads (Mtsp/Mwa stream pcm + ATR/Awa audio-track pcm) ──────────
// a good half of real-world .mmf files are mostly THIS: a tiny score that
// triggers one big adpcm sample of the actual song. wave numbers are bound
// to notes via bank-select on the stream-pcm banks.
struct WaveData {
    int      number       = 0;      // Mwa/Awa id byte (1-based in the wild)
    uint8_t  formatByte   = 0;      // raw wave-type byte 1 (channels/codec)
    uint32_t samplingRate = 0;      // decoded from wave-type bytes
    int      bitsPerSample = 4;     // 4 = yamaha adpcm, 8/16 = pcm
    int      channels     = 1;
    bool     adpcm        = true;
    std::vector<uint8_t> data;      // raw codec bytes, NOT decoded here
};

// ── one MTR*/ATR* track ─────────────────────────────────────────────────────
struct TrackChunk {
    int  trackNumber   = 0;         // 4th byte of the chunk id
    bool isAudioTrack  = false;     // ATR* (pcm-only track) vs MTR* (score)
    int  formatType    = -1;        // 0x00/0x01/0x02 (see banner)
    int  sequenceType  = 0;         // 0x00 stream / 0x01 sub-sequence
    int  durationTimeBase = 0;      // raw byte, use timeBaseMs() to decode
    int  gateTimeBase     = 0;
    std::vector<uint8_t> channelStatus; // HandyPhone: 2 B, Mobile: 16 B
    std::vector<uint8_t> setupData;     // Mtsu payload: voice exclusives
    std::vector<uint8_t> sequenceData;  // Mtsq payload: the event stream
    std::vector<WaveData> waves;        // Mtsp/Mwa (score-track pcm)

    // the timebase byte -> milliseconds-per-tick table from the spec.
    // 0x02 (4 ms) is what practically every real file uses.
    static double timeBaseMs(int raw);
};

// ── the file ────────────────────────────────────────────────────────────────
class SmafFile {
public:
    // cheap magic probe: 'MMMD' + a sane BE32 size. safe on any buffer.
    static bool looksLikeSmaf(const uint8_t* data, size_t size);

    // full parse. returns false on anything structurally broken; a file that
    // parses but has no playable track is the CALLER's problem to report.
    // tolerant of the classic real-world sins (crc missing, size off by the
    // crc trailer, unknown vendor chunks) because handsets were too.
    bool parse(const uint8_t* data, size_t size);

    // contents info (CNTI)
    int contentsClass = -1;         // 0x00 yamaha / 0x01+ vendor
    int contentsType  = -1;
    int codeType      = -1;         // charset of OPDA strings (0x01 sjis, ...)

    // OPDA/Dch metadata, decoded to utf-8 where the charset is known.
    std::string title;              // 'ST'
    std::string artist;             // 'AN' / 'AR'
    std::string writer;             // 'SW' / 'WW'
    std::string copyright;          // 'CR'
    std::string vendor;             // 'VN'

    std::vector<TrackChunk> tracks;

    // convenience: the "best" score track to play. real files carry either
    // one HandyPhone track set (MTR0-3, play them all) or one Mobile track
    // (MTR5/6/7). returns indices into tracks.
    std::vector<size_t> playableScoreTracks() const;

private:
    void parseCnti_(const uint8_t* p, size_t n);
    void parseOpda_(const uint8_t* p, size_t n);
    void parseScoreTrack_(const uint8_t* p, size_t n, int trackNo);
    void parseAudioTrack_(const uint8_t* p, size_t n, int trackNo);
    void parseWaves_(const uint8_t* p, size_t n, TrackChunk& t);
    std::string decodeText_(const uint8_t* p, size_t n) const;
};

} // namespace fxchain::smaf
