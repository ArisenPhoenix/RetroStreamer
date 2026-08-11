# Android media Package Architecture

`media/` owns Android RTP media playback.

This package parses incoming RTP H.264/Opus payloads, feeds MediaCodec/audio
playback, and reports playback health. UI surfaces should call this layer instead
of parsing media directly.
