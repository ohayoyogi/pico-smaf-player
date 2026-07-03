# roadmap

## done
- MMMD container parser (CNTI/OPDA/MTR/ATR, big-endian TLV, crc-tolerant)
- HandyPhone (MA-1/2) + Mobile (MA-3/5) event decoders
- okumura huffman inflate for the packed mobile variant
- FM voice core: 2-op/4-op operators, exponential ADSR, OPL-style algorithms
- own rom-free general-midi approximation patch bank
- VM35 (MA-3/5) + VMA (MA-1/2) voice-exclusive decode, incl. the MA-3 bit-packing
- yamaha 4-bit adpcm codec
- wav-render example CLI

## m1 (done): embedded adpcm voice playback
DONE. the Mwa/Awa wave bank is decoded once (yamaha adpcm, low-nibble-first) and
sampled voices play alongside the FM: drums one-shot at native rate, melodic pcm
pitched from a root note, loop honoured via the RM flag + LP/EP. **remaining:** a
few pure drum-kit files stash their samples in a bulk `43 79 06 7F 03` voice-wave
block instead of Mwa chunks; parsing that block is the last mile for full drum
coverage.

## m2 (done): fidelity pass
- exact 8-algorithm 4-op routing (an earlier build folded them onto four
  topologies)
- key-scaling (KSL/KSR), the voice LFO with per-operator vibrato/tremolo
  depths (DVB/DAM), XOF percussion ring-out
- the ATR audio-track wave-trigger sequence plays the sampled drums that ride
  alongside the score
- still open: verify the adpcm nibble order against real files by ear, and the
  SD-1-specific higher waveshapes

## m3: the MFi sibling
the docomo `.mld` format (magic `melo`) is a cousin. recognition + metadata is
easy; full playback is a smaller version of the same job.

## later / maybe
- a tiny real-time player (sdl/miniaudio) instead of only wav render
- a voice-bank inspector that dumps the FM patch a file uses

found a file that plays wrong, or one of these you want to take on? open an issue
and let's talk.
