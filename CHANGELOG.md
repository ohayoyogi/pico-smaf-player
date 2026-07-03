# changelog

## unreleased
- fidelity layer: the voice LFO plays for real now (vibrato + tremolo with the
  chip's per-operator DVB/DAM depths and four LFO speeds), envelopes key-scale
  with KSR, high notes thin out with KSL, and XOF percussion rings out instead
  of being chopped by short gates
- the ATR audio-track sequence plays: its wave-trigger stream (the handyphone
  grammar with the note nibble as a wave number) fires the sampled drums that
  ride alongside the score, on the score's clock
- ear round 2 (verified against the vavi-sound reference): HandyPhone notes
  play at the correct +36 base register (a +60 base chirped everything two
  octaves sharp); a channel's first bank/program assignment is hoisted to t=0
  so opening notes bind the intended voice (the chip rewrites channel registers
  mid-note, a player snapshots at note-on); MTR channel-status rhythm channels
  are decoded and unmatched drum notes fall back to a percussion hit instead of
  a melodic piano; gm-style squared velocity curve, real mix headroom and a
  transparent output peak limiter replace the tanh knee (the "overdriven"
  sound); operators landing above nyquist are muted instead of aliasing
- correctness pass against the go-smaf reference: the 8 connection algorithms
  are now routed for real (an earlier build collapsed them onto four topologies
  and mis-timed every 4-op voice); HandyPhone notes play in the right register
  (were two octaves flat); VM35 egType derives from the sustain rate, not the
  inert SUS bit; the packed MA-3 operator count reads the algorithm byte; a
  mis-indexed sampled voice falls back to the shortest wave, not a loud long one
- authentic MA-chip FM voicing: byte-exact VM35/VMA/packed voice decode, the
  8 real connection algorithms, linear MULTI, tamed modulation index + an
  analog-lite output filter (warm, not shrill)
- embedded PCM voices: yamaha-adpcm wave bank + sampled drum/instrument playback
- initial extraction from FXChainPlayer
- MMMD container parser (CNTI/OPDA/MTR/ATR)
- HandyPhone (MA-1/2) + Mobile (MA-3/5) event decoders + okumura huffman
- FM voice core (2-op/4-op, exponential ADSR, OPL-style algorithms) + own
  rom-free general-midi approximation patch bank
- VM35 / VMA voice-exclusive decode
- yamaha 4-bit adpcm codec
- wav-render example CLI
