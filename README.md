# BelkaRx

BelkaRx is an Android spectrum analyzer and waterfall display, designed for the **Belka SDR** radio (48 kHz of I/Q bandwidth). It should work with other I/Q sources too, but only the Belka has been tested.

To see the Belka's signal, connect its I/Q (or line) output to the phone through a **USB sound card with a stereo input**, using an OTG adapter. It has been tested with an Axagon ADA-17. A mono input is not enough, because I/Q needs both channels.

Without a sound card the app captures from the phone's built-in microphone. That is only useful for trying it out.

## Features

Signal processing (FFT, CW and RTTY decoding) runs in native C++ code. The waterfall is drawn on the GPU, so it scrolls smoothly at the display's refresh rate.

* **Waterfall view:** the received band at a glance, which helps with tuning and finding weak signals. The frequency scale at the top is in kHz, relative to the demodulation reference.
* **Spectrum view:** **Spectrum** switches from the waterfall to a spectrum trace. Two options are available while it is on:
  * **Fill:** a filled trace.
  * **Mono:** a single-color trace.
* **Demodulation modes:** the **AM / USB / LSB** button steps through the modes. The yellow arrow marks the reference frequency, and an orange bar shows the receive band of the mode.
* **Marker:** turn on **Marker**, or long-press the waterfall, then drag the cyan line to a signal. Its label shows the offset from the demodulation reference in Hz.
* **Zoom:** **Zoom** shows a 24 kHz wide part of the band instead of the full 48 kHz. Double-tap the waterfall to zoom in around that point, and double-tap again to zoom out.
* **Sensitivity and contrast:** two sliders set the signal level reference and the dynamic range, so that signals stand out from the noise floor.
* **Swap I/Q:** fixes a mirrored spectrum if the sound card's left and right channels are swapped.
* **Waterfall colors:** pick a color scale from the gradient swatches: Classic Rainbow, Light Blue, Grayscale, Cool-Hot, Green Phosphor or LCD. Long-press a swatch to see its name.
* **CW (Morse) decoder:** decodes Morse at the marker frequency. It shows the text, the speed (WPM) and a signal level graph at the bottom of the waterfall, and the text can be saved to a file. See [CW Decoder](#cw-decoder) below.
* **RTTY decoder:** decodes radioteletype (Baudot / ITA2), such as the DWD weather broadcasts and amateur RTTY, with a crossed-ellipse tuning scope. See [RTTY Decoder](#rtty-decoder) below.
* **More room for the waterfall:** the **▲** button hides the controls, and **▼** shows them again.
* **Portrait and landscape:** in landscape, the sliders move to the top bar.

All settings are remembered between sessions.

## Usage

1. Connect a USB OTG adapter to the phone and plug in the USB sound card.
2. Connect the Belka SDR's I/Q (or line) output to the sound card's stereo input (line in or mic).
3. Start BelkaRx and allow audio recording when asked. It is needed to read the I/Q signal.
4. Tap **Start**. The status line shows the capture format and the sample rate.
5. Adjust **Sensitivity** and **Contrast** until the signals stand out from the noise.

There is no input selector: the audio is captured from the input Android selects. When a USB sound card is plugged in, that is the sound card. Otherwise it is the built-in microphone.

## CW Decoder

The decoder works on the I/Q signal itself, not on the waterfall picture:
1. It mixes the selected tone down to 0 Hz.
2. It filters a 100 Hz wide channel.
3. It keys the tone's envelope against an adaptive threshold.
4. It estimates the dot length from the received elements.

Speeds from 5 to 50 WPM are decoded, and the speed is followed automatically.

### Using it

1. Tap **CW** (next to Spectrum / Fill / Mono). The marker turns on with it, because the decoder listens at the marker frequency.
2. Drag the marker onto a CW signal on the waterfall. When you release it, the marker snaps to the strongest signal within ±100 Hz, so it does not need to be exact. A green band shows the decoder's 100 Hz channel.
3. Turn on **Zoom** to tune more easily. In CW mode the zoom shows 6 kHz around the marker, with a scale mark every kHz. Double-tapping zooms in at the tapped point.
4. The decoded text appears in the strip at the bottom of the waterfall.

While CW is on, a long press on the waterfall does not turn the marker off, so the decoder cannot be stopped by accident. To turn the marker off, turn CW off first.

### The CW strip

* **Signal level graph:** the received tone level over the last 4 seconds, in dB above the noise floor.
  * Green line: the tone level.
  * Grey line at 0 dB: the noise floor.
  * Yellow line: the **squelch**. The tone has to reach it to be decoded.
  * Dashed green line: where the decoder cuts the keying (halfway between noise and tone). It is shown when it is above the squelch.
* **AUTO** (bottom right of the graph): the squelch is set automatically, 2.5 dB above the noise peaks measured during pauses. This is the default.
* **Manual squelch:** drag up or down anywhere on the graph to set the squelch yourself (3–30 dB). This switches AUTO off.
  * Raise it if noise is decoded as random letters (mostly `E` and `T`).
  * Lower it if a weak signal is not decoded.
* **Key indicator and WPM:** the dot is bright green while the tone is keyed, dark green while a signal is present, and grey otherwise. Below it is the estimated speed.
* **Text:** 3 lines (2 in landscape) of the decoded text. Swipe up and down to scroll back (about 10,000 characters are kept). While you read further up, new text does not move the view; scroll back to the bottom and it follows the newest text again. Prosigns appear as `<SK>`, and characters that could not be decoded as `*`.
* **Clear** empties the text. **Save** writes the whole log to a text file. The Android file picker lets you choose where to save it, so no storage permission is needed. Each transmission starts on a new line with a `[HH:mm:ss]` timestamp, after a pause of at least 10 seconds.

The CW mode, AUTO, and the manual squelch value are remembered between sessions.

### Tips and limits

* Signals about 10 dB above the noise (in the 100 Hz channel) decode reliably. Weaker ones produce errors.
* Fading (QSB) is followed, but in deep fades the signal drops below the squelch and characters are lost.
* There is no automatic frequency tracking. If the signal drifts, drag the marker again.
* If the status line shows *"raw input not supported, AGC may apply"*, the phone does not provide an unprocessed audio input. Android's automatic gain control can then raise the noise after each strong signal, which makes decoding harder.

## RTTY Decoder

The RTTY decoder reads radioteletype: two tones (mark and space) keyed at 45.45, 50 or 75 baud, 5-bit Baudot / ITA2 code with a start and a stop bit. It works on the I/Q signal, like the CW decoder:
1. It mixes the center between mark and space down to 0 Hz and filters out ±600 Hz.
2. Two filters, each about one baud wide, pick out the mark and the space tone.
3. The two levels are compared with automatic threshold correction (W7AY's "optimal ATC"), so the decision stays right when one tone fades more than the other (selective fading).
4. Each character is timed from its start bit. Every bit is decided from its middle half, and characters with a broken stop bit are dropped.

Good first targets are the German Weather Service (DWD) broadcasts, which are on the air around the clock: 50 Bd, 450 Hz shift, on 147.3, 4583, 7646, 10100.8, 11039 and 14467.3 kHz.

Real NAVTEX (518 and 490 kHz) uses SITOR-B, a different code at 100 baud, which this decoder does not read.

### Using it

1. Tap **RTTY** (next to CW). The marker turns on with it, and CW turns off: one decoder runs at a time.
2. Choose the **Baud** rate and the **Shift** in the strip at the bottom. For DWD: 50 and 450.
3. Drag the marker between the two tones of the signal. When you release it, the marker snaps to the center of the strongest tone pair within ±150 Hz. A green band shows the mark filter and an orange band the space filter.
4. Turn on **Zoom** to tune more easily: as in CW mode, it shows 6 kHz around the marker.
5. If only garbage appears on a clean signal, tap **Invert**: the station sends mark on the lower frequency (or the I/Q channels are swapped).

As in CW mode, a long press on the waterfall does not turn the marker off while RTTY is on.

### The RTTY strip

* **Tuning scope** (left): the classic crossed-ellipse display. The mark filter drives it sideways and the space filter up and down.
  * A well-tuned signal draws a **+**: a horizontal line (mark) crossed by a vertical one (space), about as long as the dashed ring.
  * One line only, or one line much shorter than the other: the marker is off center, or the shift is wrong.
  * A round blob or a diagonal smear: noise, or the tones fall between the filters.
  * The trace is bright green while the squelch is open, grey otherwise. The size is normalized, so weak and strong signals look the same size.
* **Baud:** 45 (45.45 Bd, amateur RTTY), 50 (DWD and most utility stations) or 75.
* **Shift (Hz):** 85, 170 (amateur), 425, 450 (DWD) or 850.
* **Invert:** swaps mark and space.
* **Signal dot and dB value:** the dot is grey while the squelch is closed, and green (mark) or orange (space) while a signal is decoded. Below it is the signal-to-noise estimate. Noise alone reads about 5 dB, text is printed from 7 dB, and clean signals read 12–13 dB.
* **Text:** 6 lines in portrait, 2 in landscape. Swipe up and down to scroll back (about 150 lines are kept). While you read further up, new text does not move the view; scroll back to the bottom and it follows the newest text again. Line breaks of the transmission are kept, but empty lines are dropped.
* **Clear** and **Save** work as in the CW strip. The saved log starts with the baud rate and shift in use.

The RTTY mode, baud rate, shift and Invert are remembered between sessions.

### Tips and limits

* On test signals, text is error-free from about 12 dB signal-to-noise ratio (measured in a bandwidth equal to the baud rate) and still readable around 10 dB.
* Characters are only printed while the SNR estimate is above the squelch (7 dB to open, 6 dB to close). The squelch is fixed: tested on pure noise, it lets through about one stray character in 9 minutes.
* Mistuning by 10 Hz costs little; beyond about 20 Hz the errors grow quickly. There is no automatic frequency tracking, so drag the marker again if the signal drifts.
* Letters/figures shifts are followed even while the squelch is closed. Unshift-on-space is off, which suits utility stations: a lost FIGS or LTRS can garble numbers until the next shift character.

## Technical Background

* Built with Android Studio, in Kotlin.
* Native Processing (C/C++): FFT (Fast Fourier Transform) calculations and the CW and RTTY decoders.
* Rendering: the waterfall is drawn with OpenGL ES 3 on a dedicated render thread, scrolling smoothly at the display refresh rate.
* Audio capture: Android AudioRecord with the UNPROCESSED source, stereo 16-bit, at 192 kHz (the actual rate depends on the hardware). If the phone has no unprocessed input, the status line says so, because Android's automatic gain control may then alter the signal.
* Minimum Android version: 9 (API 28).
