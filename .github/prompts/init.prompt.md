---
name: init
description: "BelkaRx project base context: Windows 10, Android NDK C++/Kotlin SDR app, Gradle build. Load this when starting any session in this workspace."
agent: agent
---

## Environment

- **OS**: Windows 10
- **Shell**: PowerShell (use `.\gradlew` not `gradlew`; chain commands with `;` not `&&`)
- **Python**: 3.10 available if needed
- **Build**: `cd c:\Users\pmate\AndroidStudioProjects\BelkaRx; .\gradlew build`

## Project: BelkaRx

An Android SDR (Software Defined Radio) receiver app with:
- **Native C++ rendering** (`app/src/main/cpp/native-lib.cpp`): FFT, waterfall, spectrum display, color scales
- **Kotlin UI** (`app/src/main/java/com/example/belkarx/MainActivity.kt`): AudioRecord capture, UI controls
- **Audio pipeline**: `AudioRecord UNPROCESSED` capture + native DSP/render pipeline

## Key Conventions

- Android pixel format is **BGRA**: color literals are `0xFFBBGGRR` (alpha, blue, green, red)
- Color scales are indexed 0–6; never shift existing indices (breaks saved user settings)
- FFT size: **2048 frames**; always accumulate full windows before calling `processAndDraw`
- `.\gradlew build` after every native C++ or Kotlin change — verify BUILD SUCCESSFUL before finishing