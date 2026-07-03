// SPDX-License-Identifier: Apache-2.0
//
// smaf_file.cpp -- the container parser. big-endian TLV, nothing clever.
// ----------------------------------------------------------------------------
// every offset in here was checked against real bytes from a ~500-file corpus
// of handset ringtones before it was trusted. the format is honest but the
// files are not always: crc trailers go missing, the outer MMMD size is off by
// the trailer, vendors stuff private chunks in the middle. a handset had to eat
// all of that without choking, so we do too.

#include "smaf_file.h"

#include <algorithm>
#include <cstring>

namespace fxchain::smaf {

namespace {

// big-endian readers. smaf is BE end to end (motorola heritage, m16c/sh cores).
inline uint32_t be32(const uint8_t* p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) |
           (uint32_t(p[2]) << 8)  |  uint32_t(p[3]);
}

// a chunk id whose first 3 chars are printable ascii. the 4th is allowed to be
// a raw track number (MTR\x05) so we only vet the first three.
inline bool idLooksSane(const uint8_t* p) {
    for (int i = 0; i < 3; ++i)
        if (p[i] < 0x20 || p[i] > 0x7e) return false;
    return true;
}

inline bool id3(const uint8_t* p, const char* s) {
    return p[0] == uint8_t(s[0]) && p[1] == uint8_t(s[1]) && p[2] == uint8_t(s[2]);
}

// shift-jis -> utf-8, just enough for the ascii-clean tag values real files
// carry. full kanji round-trip is not worth a table here; non-ascii bytes are
// passed through as latin-ish so a japanese title at least does not crash.
std::string sjisToUtf8Best(const uint8_t* p, size_t n) {
    std::string out;
    out.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        uint8_t b = p[i];
        if (b == 0) break;
        if (b < 0x80) { out.push_back(char(b)); continue; }
        // half-width katakana / lead byte: emit a placeholder rather than
        // corrupt the utf-8 stream. metadata is cosmetic; playback is not.
        out.push_back('?');
        // skip a trail byte for a double-byte lead range
        if ((b >= 0x81 && b <= 0x9f) || (b >= 0xe0 && b <= 0xfc)) {
            if (i + 1 < n && p[i + 1] != 0) ++i;
        }
    }
    return out;
}

} // namespace

// the timebase byte -> ms-per-tick map. the spec enumerates a small set; 0x02
// (4 ms) is what essentially every real MA file uses. unknown values fall back
// to 4 ms so a weird file still plays at a sane speed instead of silent.
double TrackChunk::timeBaseMs(int raw) {
    switch (raw) {
        case 0x00: return 1.0;
        case 0x01: return 2.0;
        case 0x02: return 4.0;
        case 0x03: return 5.0;
        case 0x10: return 1.0;   // seen in the wild alongside 0x11
        case 0x11: return 2.0;
        case 0x12: return 4.0;
        case 0x13: return 5.0;
        default:   return 4.0;
    }
}

bool SmafFile::looksLikeSmaf(const uint8_t* d, size_t n) {
    if (!d || n < 12) return false;
    if (!(d[0] == 'M' && d[1] == 'M' && d[2] == 'M' && d[3] == 'D')) return false;
    uint32_t sz = be32(d + 4);
    // the declared size must be plausible: it covers everything after the
    // 8-byte MMMD header, minus the CRC trailer. allow slack both ways.
    return sz >= 8 && sz <= n; // trailer + off-by-crc tolerated
}

std::string SmafFile::decodeText_(const uint8_t* p, size_t n) const {
    // codeType 0x00/0x01 = shift-jis in practice; anything else we treat as
    // ascii/latin passthrough (european handset dumps are plain ascii).
    if (codeType <= 0x01) return sjisToUtf8Best(p, n);
    std::string s;
    for (size_t i = 0; i < n && p[i]; ++i) s.push_back(char(p[i]));
    return s;
}

void SmafFile::parseCnti_(const uint8_t* p, size_t n) {
    // contents info: class, type, codeType, copy-status, copy-count ...
    // only the first three bytes matter to us; the rest is DRM bookkeeping
    // that never affected playback.
    if (n >= 1) contentsClass = p[0];
    if (n >= 2) contentsType  = p[1];
    if (n >= 3) codeType      = p[2];
}

void SmafFile::parseOpda_(const uint8_t* p, size_t n) {
    // OPDA holds one or more Dch* (data-chunk) sub-chunks. each Dch carries a
    // run of "KEY value" fields; the classic ones are 2-letter tags. we walk
    // the sub-chunks and, inside each, scan for the known 2-char tags followed
    // by their length-prefixed value. real files are loose about this, so we
    // stay defensive and never read past n.
    size_t pos = 0;
    while (pos + 8 <= n) {
        const uint8_t* c = p + pos;
        if (!id3(c, "Dch")) { ++pos; continue; }   // resync on garbage
        uint32_t sz = be32(c + 4);
        size_t body = pos + 8;
        if (body + sz > n) sz = uint32_t(n - body);
        // scan the Dch body for 2-letter tag + BE16 length + value.
        const uint8_t* b = p + body;
        size_t bn = sz, i = 0;
        while (i + 4 <= bn) {
            char k0 = char(b[i]), k1 = char(b[i + 1]);
            // a tag key is two uppercase ascii letters.
            bool isKey = (k0 >= 'A' && k0 <= 'Z') && (k1 >= 'A' && k1 <= 'Z');
            if (!isKey) { ++i; continue; }
            uint32_t vlen = (uint32_t(b[i + 2]) << 8) | b[i + 3];
            size_t vpos = i + 4;
            if (vpos + vlen > bn) { ++i; continue; }
            std::string val = decodeText_(b + vpos, vlen);
            const char key[3] = { k0, k1, 0 };
            if      (!std::strcmp(key, "ST")) title     = val;
            else if (!std::strcmp(key, "AN")) artist    = val;
            else if (!std::strcmp(key, "AR")) { if (artist.empty()) artist = val; }
            else if (!std::strcmp(key, "SW")) writer    = val;
            else if (!std::strcmp(key, "WW")) { if (writer.empty()) writer = val; }
            else if (!std::strcmp(key, "CR")) copyright = val;
            else if (!std::strcmp(key, "VN")) vendor    = val;
            i = vpos + vlen;
        }
        pos = body + sz;
    }
}

void SmafFile::parseWaves_(const uint8_t* p, size_t n, TrackChunk& t) {
    // Mtsp (stream pcm) / a bare wave region contains MspI (info) + Mwa* wave
    // payloads (score track), or for ATR it is Atsq + Awa*. we harvest the raw
    // Mwa/Awa bytes + their format byte; decode happens later in the mixer.
    size_t pos = 0;
    while (pos + 8 <= n) {
        const uint8_t* c = p + pos;
        bool isMwa = id3(c, "Mwa");
        bool isAwa = id3(c, "Awa");
        if (!isMwa && !isAwa) { ++pos; continue; }
        uint32_t sz = be32(c + 4);
        size_t body = pos + 8;
        if (body + sz > n) sz = uint32_t(n - body);
        WaveData w;
        w.number = c[3];                 // 4th id byte = wave number
        if (sz >= 2) {
            // wave-type: [formatByte][more]. the format byte packs base-fmt +
            // channels; the sampling rate follows in the next byte(s) in the
            // full spec. we keep the raw bytes and a best-effort decode.
            w.formatByte = p[body];
            uint8_t fmt2 = (sz >= 2) ? p[body + 1] : 0;
            // low nibble of fmt2 tends to encode the rate class; adpcm = 4-bit.
            w.adpcm = true;   // MA phone audio is overwhelmingly yamaha adpcm
            w.bitsPerSample = 4;
            w.channels = 1;
            // sampling-rate class table (spec): 0=4k,1=8k,2=11.025k,3=22.05k...
            switch (fmt2 & 0x0f) {
                case 0: w.samplingRate = 4000;  break;
                case 1: w.samplingRate = 8000;  break;
                case 2: w.samplingRate = 11025; break;
                case 3: w.samplingRate = 22050; break;
                case 4: w.samplingRate = 44100; break;
                default: w.samplingRate = 8000; break;
            }
            size_t dataOff = body + 2;
            if (dataOff < body + sz)
                w.data.assign(p + dataOff, p + body + sz);
        }
        t.waves.push_back(std::move(w));
        pos = body + sz;
    }
}

void SmafFile::parseScoreTrack_(const uint8_t* p, size_t n, int trackNo) {
    if (n < 4) return;
    TrackChunk t;
    t.trackNumber  = trackNo;
    t.isAudioTrack = false;
    t.formatType   = p[0];
    t.sequenceType = p[1];
    t.durationTimeBase = p[2];
    t.gateTimeBase     = p[3];

    // channel-status width depends on the generation:
    //   fmt 0x00 HandyPhone : 2 bytes  (header total 6)
    //   fmt 0x01/0x02 Mobile: 16 bytes (header total 20)
    //   fmt 0x03            : 32 bytes (header total 36)
    // verified by measuring the first sub-chunk offset across the whole corpus.
    size_t chStatusLen = (t.formatType == 0x00) ? 2
                       : (t.formatType == 0x03) ? 32
                       : 16;
    size_t hdr = 4 + chStatusLen;
    if (hdr > n) hdr = n;
    t.channelStatus.assign(p + 4, p + hdr);

    // walk the inner sub-chunks: Mtsu (setup/voices), Mtsq (sequence),
    // Mtsp (stream pcm block), MspI (stream info). unknown ids are skipped.
    size_t pos = hdr;
    while (pos + 8 <= n) {
        const uint8_t* c = p + pos;
        if (!idLooksSane(c)) { ++pos; continue; }
        uint32_t sz = be32(c + 4);
        size_t body = pos + 8;
        if (body + sz > n) sz = uint32_t(n - body);
        if      (id3(c, "Mts") && c[3] == 'u') t.setupData.assign(p + body, p + body + sz);
        else if (id3(c, "Mts") && c[3] == 'q') t.sequenceData.assign(p + body, p + body + sz);
        else if (id3(c, "Mts") && c[3] == 'p') parseWaves_(p + body, sz, t);
        pos = body + sz;
        if (sz == 0) { ++pos; }
    }
    tracks.push_back(std::move(t));
}

void SmafFile::parseAudioTrack_(const uint8_t* p, size_t n, int trackNo) {
    if (n < 4) return;
    TrackChunk t;
    t.trackNumber  = trackNo;
    t.isAudioTrack = true;
    t.formatType   = p[0];
    t.sequenceType = p[1];
    // ATR header: fmt, seqType, timebase(?), then wave-type, then Atsq + Awa*.
    // audio tracks are a secondary path (a score track usually already carries
    // the song via Mtsp). we harvest waves + sequence for completeness.
    size_t pos = 4;
    while (pos + 8 <= n) {
        const uint8_t* c = p + pos;
        if (!idLooksSane(c)) { ++pos; continue; }
        uint32_t sz = be32(c + 4);
        size_t body = pos + 8;
        if (body + sz > n) sz = uint32_t(n - body);
        if      (id3(c, "Ats") && c[3] == 'q') t.sequenceData.assign(p + body, p + body + sz);
        else if (id3(c, "Awa"))                parseWaves_(c, 8 + size_t(sz), t); // exactly THIS chunk
        pos = body + sz;
        if (sz == 0) { ++pos; }
    }
    tracks.push_back(std::move(t));
}

bool SmafFile::parse(const uint8_t* d, size_t n) {
    tracks.clear();
    title.clear(); artist.clear(); writer.clear(); copyright.clear(); vendor.clear();
    contentsClass = contentsType = codeType = -1;

    if (!looksLikeSmaf(d, n)) return false;

    uint32_t declared = be32(d + 4);
    // the payload region: after MMMD header, bounded by declared size (which
    // excludes the crc trailer) but never past the actual buffer.
    size_t end = std::min<size_t>(size_t(8) + declared, n);

    size_t pos = 8;
    while (pos + 8 <= end) {
        const uint8_t* c = d + pos;
        if (!idLooksSane(c)) { ++pos; continue; }
        uint32_t sz = be32(c + 4);
        size_t body = pos + 8;
        if (body + sz > n) sz = uint32_t(n - body);   // clamp to real bytes

        if      (id3(c, "CNT")) parseCnti_(d + body, sz);
        else if (id3(c, "OPD")) parseOpda_(d + body, sz);
        else if (id3(c, "MTR")) parseScoreTrack_(d + body, sz, c[3]);
        else if (id3(c, "ATR")) parseAudioTrack_(d + body, sz, c[3]);
        // MTSU/MTSP at top-level, MMMD-nested vendor chunks, etc. -> skipped.

        pos = body + sz;
        if (sz == 0) { ++pos; }   // never spin on a zero-size chunk
    }

    // a file with at least one track (score or audio) is a successful parse;
    // "nothing playable" is a judgement the caller makes via playableScoreTracks.
    return !tracks.empty();
}

std::vector<size_t> SmafFile::playableScoreTracks() const {
    std::vector<size_t> out;
    // prefer score tracks with an actual sequence. if a file is pure ATR audio
    // (rare), fall back to the audio tracks so it still makes sound.
    for (size_t i = 0; i < tracks.size(); ++i)
        if (!tracks[i].isAudioTrack && !tracks[i].sequenceData.empty())
            out.push_back(i);
    if (out.empty())
        for (size_t i = 0; i < tracks.size(); ++i)
            if (tracks[i].isAudioTrack) out.push_back(i);
    return out;
}

} // namespace fxchain::smaf
