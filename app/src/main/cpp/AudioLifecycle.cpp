#include "AudioLifecycle.h"

#include <cstdint>
#include <unistd.h>
#include <vector>

#include <android/log.h>
#include <android/native_window_jni.h>

#include "NativeShared.h"

#define LOG_TAG "BelkaRx-Native"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)

namespace {

struct AudioThreadEnv {
    JNIEnv* env;
    jshortArray frameArray;
    int16_t* buffer;
    bool attached;
};

bool setupAudioEnvironment(AudioThreadEnv& audio) {
    audio.env = nullptr;
    audio.attached = false;
    audio.frameArray = nullptr;
    audio.buffer = nullptr;

    if (g_vm->GetEnv((void**)&audio.env, JNI_VERSION_1_6) == JNI_EDETACHED) {
        g_vm->AttachCurrentThread(&audio.env, nullptr);
        audio.attached = true;
    }
    if (audio.env == nullptr) {
        LOGI("ERROR: Failed to get JNI environment");
        return false;
    }

    const int32_t fftSize = 2048;
    audio.buffer = new int16_t[fftSize * 2 * 4];

    audio.frameArray = audio.env->NewShortArray(fftSize * 2);
    if (audio.frameArray == nullptr) {
        LOGI("ERROR: Failed to create frame array");
        delete[] audio.buffer;
        if (audio.attached) g_vm->DetachCurrentThread();
        return false;
    }
    return true;
}

void cleanupAudioEnvironment(AudioThreadEnv& audio) {
    if (audio.frameArray != nullptr) {
        audio.env->DeleteLocalRef(audio.frameArray);
        audio.frameArray = nullptr;
    }
    delete[] audio.buffer;
    audio.buffer = nullptr;
    if (audio.attached) {
        g_vm->DetachCurrentThread();
        audio.attached = false;
    }
}

void audioReadingThread() {
    if (g_audioCapture == nullptr || !g_isCapturing) return;

    AudioThreadEnv audio{};
    if (!setupAudioEnvironment(audio)) return;

    const int32_t fftSize = 2048;
    constexpr double kTargetRenderFps = 60.0;
    double renderBudget = 0.0;
    std::vector<int16_t> accumulator;
    accumulator.reserve(fftSize * 2);

    while (g_isCapturing && g_audioCapture != nullptr) {
        int32_t framesRead = g_audioCapture->readFrames(audio.buffer, fftSize);
        if (framesRead < 0) {
            LOGI("AAudio read error");
            break;
        }
        if (framesRead == 0) {
            usleep(10);
            continue;
        }

        int32_t newSamples = framesRead * 2;
        accumulator.insert(accumulator.end(), audio.buffer, audio.buffer + newSamples);
        if ((int32_t)accumulator.size() < fftSize * 2) {
            continue;
        }

        audio.env->SetShortArrayRegion(audio.frameArray, 0, fftSize * 2, accumulator.data());

        if ((int32_t)accumulator.size() > fftSize * 2) {
            accumulator.erase(accumulator.begin(), accumulator.begin() + fftSize * 2);
        } else {
            accumulator.clear();
        }

        bool shouldRender = true;
        jobject surface = nullptr;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            if (!showSpectrumEnabled) {
                double sourceWindowsPerSecond = static_cast<double>(currentSampleRate) / fftSize;
                if (sourceWindowsPerSecond > kTargetRenderFps) {
                    renderBudget += kTargetRenderFps;
                    if (renderBudget >= sourceWindowsPerSecond) {
                        renderBudget -= sourceWindowsPerSecond;
                    } else {
                        shouldRender = false;
                    }
                }
            }
            surface = g_surfaceRef;
        }
        if (!shouldRender) {
            continue;
        }

        Java_hu_ha8mz_belkarx_MainActivity_processAndDraw(audio.env, nullptr, audio.frameArray, fftSize * 2, surface);
    }

    cleanupAudioEnvironment(audio);
}

void clearSurfaceRefs(JNIEnv* env) {
    if (g_surfaceRef != nullptr) {
        env->DeleteGlobalRef(g_surfaceRef);
        g_surfaceRef = nullptr;
    }
    if (g_nativeWindow != nullptr) {
        ANativeWindow_release(g_nativeWindow);
        g_nativeWindow = nullptr;
    }
}

}  // namespace

bool startOboeCaptureImpl(JNIEnv* env, jobject activity, jint deviceId, jint sampleRate) {
    if (g_audioCapture != nullptr) {
        LOGI("Oboe capture already running");
        return false;
    }

    env->GetJavaVM(&g_vm);
    g_mainActivityRef = env->NewGlobalRef(activity);

    LOGI("Starting Oboe capture: deviceId=%d, sampleRate=%d", deviceId, sampleRate);

    g_audioCapture = new OboeCapture();
    if (!g_audioCapture->start(deviceId, sampleRate)) {
        LOGI("Failed to start Oboe capture");
        delete g_audioCapture;
        g_audioCapture = nullptr;
        if (g_mainActivityRef != nullptr) {
            env->DeleteGlobalRef(g_mainActivityRef);
            g_mainActivityRef = nullptr;
        }
        return false;
    }

    g_isCapturing = true;
    g_audioThread = new std::thread(audioReadingThread);
    LOGI("Oboe capture started successfully");
    return true;
}

void stopOboeCaptureImpl(JNIEnv* env) {
    if (g_audioCapture == nullptr) return;

    LOGI("Stopping Oboe capture");
    g_isCapturing = false;

    if (g_audioThread != nullptr) {
        g_audioThread->join();
        delete g_audioThread;
        g_audioThread = nullptr;
    }

    g_audioCapture->stop();
    delete g_audioCapture;
    g_audioCapture = nullptr;

    if (g_mainActivityRef != nullptr) {
        env->DeleteGlobalRef(g_mainActivityRef);
        g_mainActivityRef = nullptr;
    }
    clearSurfaceRefs(env);
    LOGI("Oboe capture stopped");
}

void setOboeSurfaceImpl(JNIEnv* env, jobject surface) {
    std::lock_guard<std::mutex> lock(g_mutex);
    clearSurfaceRefs(env);

    if (surface != nullptr) {
        g_surfaceRef = env->NewGlobalRef(surface);
        g_nativeWindow = ANativeWindow_fromSurface(env, surface);
        LOGI("setOboeSurface: Updated to new surface reference %p", surface);
    } else {
        LOGI("setOboeSurface: Cleared surface reference");
    }
}
