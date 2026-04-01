# BelkaRx

This application is an Android spectrum analyzer and waterfall display designed specifically for the **Belka SDR** radio. It allows you to visually display and analyze the received signal spectrum on an Android smartphone. (48 KHz bandwidth). It could work with other IQ signals too, but I didn't tested yet.

**⚠️ IMPORTANT: A USB sound card is required for use!** 
The IQ (or line) output of the Belka SDR radio must be connected to the phone via this USB sound card (using an OTG adapter). Tested with Axagon ADA-17. It's important that it has a stereo microphone input!

## Features and Capabilities

The program provides real-time, low-latency display thanks to fast C++ based signal processing (JNI + native renderer).

* **Spectrum and Waterfall View:** Visually see the signals in the band, which greatly helps with tuning and finding weak signals.
* **Swap I/Q Channels:** In case the left/right channels of the USB sound card are swapped (resulting in a mirrored spectrum), this can be corrected with a single button press.
* **Multiple Color Themes (Color Scale):** Customizable waterfall appearance. Available options:
  * *Classic Rainbow* (Traditional SDR colors)
  * *Light Blue*
  * *Grayscale*
  * *Cool-Hot*
* **Zoom Mode:** Ability to zoom in on a specific part of the spectrum (e.g., around the +8 kHz range for finer details).
* **Sensitivity and Contrast Adjustments:** Use sliders to adjust the signal level reference and dynamic range, allowing you to highlight signals from the noise (or hide the noise floor).
* **Fast Waterfall:** Speeds up the waterfall scrolling rate, making it easier to observe fast-changing signals (like CW/Morse).

## Technical Background

* Built with: Android Studio, Kotlin / Java
* Native Processing (C/C++): Used for FFT (Fast Fourier Transform) calculations and efficient, rapid rendering of pixels (waterfall).
* Audio Capture: Android AudioRecord (UNPROCESSED) with native DSP pipeline.
* Supported Sample Rates: Up to 96 kHz (hardware dependent).

## Usage

1. Connect a USB OTG cable or adapter to your phone.
2. Plug in the USB sound card.
3. Use an audio cable to connect the Belka SDR's I/Q (or Line Out) output to the input of the USB sound card (Line-in or Mic).
4. Launch the BelkaRx app. Grant permission to use the microphone/audio recording.
5. Select the USB audio device in the app and start signal processing! You can then fine-tune the spectrum display using the sliders.
