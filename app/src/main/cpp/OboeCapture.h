#pragma once

#include <aaudio/AAudio.h>
#include <android/log.h>
#include <cstring>
#include <queue>
#include <mutex>

#define LOG_TAG_OBOE "BelkaRx-Oboe"
#define LOGI_OBOE(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG_OBOE, __VA_ARGS__)
#define LOGE_OBOE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG_OBOE, __VA_ARGS__)

/**
 * Oboe-like wrapper over AAudio for stereo audio capture.
 * Requests stereo (2 channels) at the specified sample rate.
 */
class OboeCapture {
public:
    OboeCapture() : stream(nullptr), deviceId(AAUDIO_UNSPECIFIED) {}

    ~OboeCapture() {
        stop();
    }

    /**
     * Start capturing audio from the specified device ID.
     * @param inDeviceId Device ID (use AAUDIO_UNSPECIFIED for default)
     * @param sampleRate Desired sample rate (e.g., 48000, 192000)
     * @return true if successful, false otherwise
     */
    bool start(int32_t inDeviceId, int32_t sampleRate) {
        if (stream != nullptr) {
            LOGE_OBOE("Stream already running");
            return false;
        }

        deviceId = inDeviceId;

        AAudioStreamBuilder* builder = nullptr;
        aaudio_result_t result = AAudio_createStreamBuilder(&builder);
        if (result != AAUDIO_OK) {
            LOGE_OBOE("Failed to create stream builder: %s", AAudio_convertResultToText(result));
            return false;
        }

        // Request input stream
        AAudioStreamBuilder_setDirection(builder, AAUDIO_DIRECTION_INPUT);

        // Request stereo (2 channels)
        AAudioStreamBuilder_setChannelCount(builder, 2);

        // Request PCM 16-bit format
        AAudioStreamBuilder_setFormat(builder, AAUDIO_FORMAT_PCM_I16);

        // Set sample rate
        AAudioStreamBuilder_setSampleRate(builder, sampleRate);

        // Set device ID (if not AAUDIO_UNSPECIFIED)
        if (deviceId != AAUDIO_UNSPECIFIED) {
            AAudioStreamBuilder_setDeviceId(builder, deviceId);
        }

        // Request low latency and unprocessed audio
        AAudioStreamBuilder_setPerformanceMode(builder, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
        AAudioStreamBuilder_setUsage(builder, AAUDIO_USAGE_VOICE_COMMUNICATION);
        
        // Try to set input preset to UNPROCESSED (API level 28+)
        // AAUDIO_INPUT_PRESET_UNPROCESSED = 9
        #ifdef AAUDIO_INPUT_PRESET_UNPROCESSED
        AAudioStreamBuilder_setInputPreset(builder, AAUDIO_INPUT_PRESET_UNPROCESSED);
        #endif

        // Create the stream
        result = AAudioStreamBuilder_openStream(builder, &stream);
        AAudioStreamBuilder_delete(builder);

        if (result != AAUDIO_OK) {
            LOGE_OBOE("Failed to open stream: %s", AAudio_convertResultToText(result));
            stream = nullptr;
            return false;
        }

        // Check what we actually got
        int32_t actualChannels = AAudioStream_getChannelCount(stream);
        int32_t actualRate = AAudioStream_getSampleRate(stream);
        int32_t actualFormat = AAudioStream_getFormat(stream);
        int32_t actualDevice = AAudioStream_getDeviceId(stream);

        LOGI_OBOE("Stream opened: channels=%d, sampleRate=%d, format=%d, deviceId=%d",
             actualChannels, actualRate, actualFormat, actualDevice);

        if (actualChannels != 2) {
            LOGE_OBOE("WARNING: Requested 2 channels but got %d", actualChannels);
        }

        // Start recording
        result = AAudioStream_requestStart(stream);
        if (result != AAUDIO_OK) {
            LOGE_OBOE("Failed to start stream: %s", AAudio_convertResultToText(result));
            AAudioStream_close(stream);
            stream = nullptr;
            return false;
        }

        LOGI_OBOE("Oboe (AAudio) capture started successfully");
        return true;
    }

    /**
     * Stop capturing audio.
     */
    void stop() {
        if (stream == nullptr) return;

        AAudioStream_requestStop(stream);
        AAudioStream_close(stream);
        stream = nullptr;
        LOGI_OBOE("Oboe (AAudio) capture stopped");
    }

    /**
     * Read audio samples from the stream.
     * @param buffer Output buffer for int16_t samples (stereo interleaved: L, R, L, R, ...)
     * @param numFrames Number of frames to read (each frame = 2 samples for stereo)
     * @return Number of frames actually read, or negative on error
     */
    int32_t readFrames(int16_t* buffer, int32_t numFrames) {
        if (stream == nullptr) {
            return -1;
        }

        int32_t framesRead = AAudioStream_read(stream, buffer, numFrames, 0);
        if (framesRead < 0) {
            LOGE_OBOE("AAudioStream_read error: %s", AAudio_convertResultToText((aaudio_result_t)framesRead));
            return -1;
        }

        return framesRead;
    }

    /**
     * Check if the stream is running.
     */
    bool isRunning() const {
        if (stream == nullptr) return false;
        aaudio_stream_state_t state = AAudioStream_getState(stream);
        return state == AAUDIO_STREAM_STATE_STARTED;
    }

    /**
     * Get actual channel count of the stream.
     */
    int32_t getChannelCount() const {
        if (stream == nullptr) return 0;
        return AAudioStream_getChannelCount(stream);
    }

    /**
     * Get actual sample rate of the stream.
     */
    int32_t getSampleRate() const {
        if (stream == nullptr) return 0;
        return AAudioStream_getSampleRate(stream);
    }

    /**
     * Get device ID of the stream.
     */
    int32_t getDeviceId() const {
        if (stream == nullptr) return AAUDIO_UNSPECIFIED;
        return AAudioStream_getDeviceId(stream);
    }

private:
    AAudioStream* stream;
    int32_t deviceId;
};

