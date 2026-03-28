# Native DSP/Render Notes

## Files
- `native-lib.cpp`: JNI entrypoints, FFT pipeline orchestration, waterfall/spectrum frame production, Oboe lifecycle.
- `NativeShared.h`: Shared native globals and cross-module JNI declarations.
- `AudioLifecycle.h/.cpp`: Oboe startup/shutdown, surface ownership, and audio reading thread lifecycle.
- `ColorScales.h/.cpp`: Color mapping and LUT construction for all supported scales.
- `DspHelpers.h/.cpp`: Shared DSP utilities for IQ unpacking, span selection, and per-pixel magnitude generation.
- `RenderHelpers.h/.cpp`: Shared frame rasterization for spectrum and waterfall blit paths.
- `OboeCapture.h`: Audio device capture wrapper.

## Runtime flow
1. Kotlin feeds IQ audio blocks into `processAudioData(...)`.
2. Native side unpacks I/Q, runs FFT, computes log magnitudes.
3. If waterfall mode is active, the ring buffer is updated and `g_renderHeadSnapshot` is published.
4. UI thread `renderFrame(...)` blits either waterfall or spectrum from shared state.

## Spectrum fast-mode rule
- In fast mode, spectrum rendering uses the current measured signal directly.
- Decay buffer is kept in sync but is bypassed for plotting in fast mode.
- In normal mode, plotting uses the classic decay path.

## Threading notes
- `g_mutex`: guards mutable render/audio settings and waterfall buffer state.
- `g_spectrumMutex`: guards spectrum line magnitudes shared between audio/render threads.
- `g_renderHeadSnapshot`: atomic row index used by render thread for consistent waterfall reads.
