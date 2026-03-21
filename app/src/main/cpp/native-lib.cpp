#include <jni.h>
#include <string>
#include <vector>
#include <cmath>
#include <mutex>
#include <thread>
#include <unistd.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <android/log.h>
#include "OboeCapture.h"

#define LOG_TAG "BelkaRx-Native"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)

std::mutex g_mutex;
int surfaceWidth = 0;
int surfaceHeight = 0;
std::vector<uint32_t> waterfallBuffer;
int sensitivityValue = 100;
int currentSampleRate = 96000;
bool swapIQEnabled = false;
bool monoModeEnabled = false;  // If true, use only left channel (mono)
bool stereoAverageEnabled = false;  // If true, average L+R channels; if false, use I/Q magnitude

// Oboe capture state
OboeCapture* g_audioCapture = nullptr;
std::thread* g_audioThread = nullptr;
bool g_isCapturing = false;

// Global reference for calling back to Java processAndDraw
static JavaVM* g_vm = nullptr;
static jobject g_mainActivityRef = nullptr;
static jobject g_surfaceRef = nullptr;

void fft(std::vector<double>& real, std::vector<double>& imag) {
    int n = real.size();
    if (n <= 1) return;

    std::vector<double> real_even(n / 2), imag_even(n / 2);
    std::vector<double> real_odd(n / 2), imag_odd(n / 2);

    for (int i = 0; i < n / 2; i++) {
        real_even[i] = real[2 * i];
        imag_even[i] = imag[2 * i];
        real_odd[i] = real[2 * i + 1];
        imag_odd[i] = imag[2 * i + 1];
    }

    fft(real_even, imag_even);
    fft(real_odd, imag_odd);

    for (int i = 0; i < n / 2; i++) {
        double angle = -2.0 * M_PI * i / n;
        double wr = cos(angle);
        double wi = sin(angle);
        double tr = wr * real_odd[i] - wi * imag_odd[i];
        double ti = wr * imag_odd[i] + wi * real_odd[i];
        real[i] = real_even[i] + tr;
        imag[i] = imag_even[i] + ti;
        real[i + n / 2] = real_even[i] - tr;
        imag[i + n / 2] = imag_even[i] - ti;
    }
}

extern "C" JNIEXPORT void JNICALL
Java_com_example_belkarx_MainActivity_setSurfaceSize(JNIEnv* env, jobject /* this */, jint width, jint height) {
    std::lock_guard<std::mutex> lock(g_mutex);
    surfaceWidth = width;
    surfaceHeight = height;
    waterfallBuffer.assign(width * height, 0xFF000000);
}

extern "C" JNIEXPORT void JNICALL
Java_com_example_belkarx_MainActivity_setSensitivity(JNIEnv* env, jobject /* this */, jint value) {
    std::lock_guard<std::mutex> lock(g_mutex);
    sensitivityValue = value;
}

extern "C" JNIEXPORT void JNICALL
Java_com_example_belkarx_MainActivity_setNativeSampleRate(JNIEnv* env, jobject /* this */, jint rate) {
    std::lock_guard<std::mutex> lock(g_mutex);
    currentSampleRate = rate;
}

extern "C" JNIEXPORT void JNICALL
Java_com_example_belkarx_MainActivity_setSwapIQ(JNIEnv* env, jobject /* this */, jboolean swap) {
    std::lock_guard<std::mutex> lock(g_mutex);
    swapIQEnabled = swap;
    LOGI("setSwapIQ: %d", (int)swap);
}

extern "C" JNIEXPORT void JNICALL
Java_com_example_belkarx_MainActivity_setMonoMode(JNIEnv* env, jobject /* this */, jboolean mono) {
    std::lock_guard<std::mutex> lock(g_mutex);
    monoModeEnabled = mono;
    LOGI("setMonoMode: %d", (int)mono);
}

extern "C" JNIEXPORT void JNICALL
Java_com_example_belkarx_MainActivity_setStereoAverageMode(JNIEnv* env, jobject /* this */, jboolean stereo) {
    std::lock_guard<std::mutex> lock(g_mutex);
    stereoAverageEnabled = stereo;
    LOGI("setStereoAverageMode: %d", (int)stereo);
}

uint32_t getColor(double magnitude) {
    // magnitude comes as logMag in dB scale: 20*log10(mag)+120
    // Typical range observed: 100-160 dB
    // We need to map this full observed range to 0-255 for better contrast
    
    double intensity = magnitude;
    
    // If this looks like a logMag value (>60 indicates dB scale with our 120 offset)
    if (magnitude > 60) {
        // Map typical range 90-170 to 0-255 for maximum contrast
        // This gives us full use of the color palette
        intensity = (magnitude - 90.0) / 80.0 * 255.0;
        if (intensity < 0) intensity = 0;
        if (intensity > 255) intensity = 255;
    }
    
    // Apply sensitivity adjustment (subtle)
    intensity = intensity + (sensitivityValue - 100) * 0.3;
    if (intensity < 0) intensity = 0;
    if (intensity > 255) intensity = 255;

    int intensityInt = (int)intensity;
    
    if (intensityInt < 128) {
        // Blue -> Green gradient
        return 0xFF000000 | (intensityInt * 2 << 8) | (intensityInt > 64 ? (intensityInt - 64) * 4 : 0);
    } else {
        // Green -> Red gradient
        return 0xFF000000 | (255 << 16) | ((255 - (intensityInt - 128) * 2) << 8);
    }
}

extern "C" JNIEXPORT void JNICALL
Java_com_example_belkarx_MainActivity_processAndDraw(JNIEnv* env, jobject /* this */, jshortArray data, jint size, jobject surface) {
    if (surface == nullptr) return;

    jshort* buffer = env->GetShortArrayElements(data, nullptr);
    if (buffer == nullptr) return;
    
    static bool loggedRate = false;
    if (!loggedRate) {
        loggedRate = true;
        LOGI("processAndDraw first call: currentSampleRate=%d, size=%d", currentSampleRate, size);
    }

    int fftSize = 2048;  // Larger FFT for better frequency resolution
    if (size < 2 * fftSize) {
        env->ReleaseShortArrayElements(data, buffer, JNI_ABORT);
        return;
    }

    std::vector<double> real(fftSize);
    std::vector<double> imag(fftSize);

    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (monoModeEnabled) {
            // MONO MODE: Use only left channel (I component only, imaginary = 0)
            for (int i = 0; i < fftSize; i++) {
                real[i] = buffer[2 * i] / 32768.0;  // Left channel
                imag[i] = 0.0;                      // No imaginary part
            }
        } else if (swapIQEnabled) {
            for (int i = 0; i < fftSize; i++) {
                // I/Q handling: real = Q, imag = I
                real[i] = buffer[2 * i + 1] / 32768.0;
                imag[i] = buffer[2 * i] / 32768.0;
            }
        } else {
            for (int i = 0; i < fftSize; i++) {
                // Stereo interpretation: left = I (Real), right = Q (Imaginary)
                // Combine stereo channels: average them for magnitude computation
                double leftSample = buffer[2 * i] / 32768.0;
                double rightSample = buffer[2 * i + 1] / 32768.0;
                
                // For SDR: treat left as I and right as Q (quadrature)
                real[i] = leftSample;
                imag[i] = rightSample;
            }
        }
        
        // Debug: log input data samples
        static int inputLogCount = 0;
        if (++inputLogCount % 100 == 0) {
            int16_t minI = 32767, maxI = -32768, minQ = 32767, maxQ = -32768;
            for (int i = 0; i < fftSize; i++) {
                int16_t I_val = buffer[2 * i];  // Raw buffer, not normalized
                int16_t Q_val = buffer[2 * i + 1];
                minI = (I_val < minI) ? I_val : minI;
                maxI = (I_val > maxI) ? I_val : maxI;
                minQ = (Q_val < minQ) ? Q_val : minQ;
                maxQ = (Q_val > maxQ) ? Q_val : maxQ;
            }
            LOGI("FFT Input RAW: I[min=%d, max=%d, range=%d], Q[min=%d, max=%d, range=%d]", 
                 minI, maxI, maxI-minI, minQ, maxQ, maxQ-minQ);
        }
    }
    env->ReleaseShortArrayElements(data, buffer, JNI_ABORT);

    fft(real, imag);
    
    // Check FFT output immediately
    static int fftCheckCount = 0;
    if (++fftCheckCount % 20 == 0) {
        double mag0 = sqrt(real[0]*real[0] + imag[0]*imag[0]);
        double mag128 = sqrt(real[128]*real[128] + imag[128]*imag[128]);
        double mag512 = sqrt(real[512]*real[512] + imag[512]*imag[512]);
        
        LOGI("FFT Check #%d [MONO=%d SWAP=%d STEREO_AVG=%d]: mag[0]=%.6f, mag[128]=%.6f, mag[512]=%.6f, real[0]=%.6f, imag[0]=%.6f",
             fftCheckCount, (int)monoModeEnabled, (int)swapIQEnabled, (int)stereoAverageEnabled, mag0, mag128, mag512, real[0], imag[0]);
        
        // Mono mode validation: imag should be 0 or very close
        if (monoModeEnabled) {
            double imagMag = fabs(imag[0]);
            if (imagMag > 0.001) {
                LOGI("WARNING: Mono mode enabled but imag[0]=%.6f (not zero!)", imag[0]);
            }
        }
    }

    std::lock_guard<std::mutex> lock(g_mutex);
    if (surfaceWidth <= 0 || surfaceHeight <= 0 || waterfallBuffer.size() != (size_t)(surfaceWidth * surfaceHeight)) {
        return;
    }

    // Shift waterfall down
    for (int y = surfaceHeight - 1; y > 0; y--) {
        memcpy(&waterfallBuffer[y * surfaceWidth], &waterfallBuffer[(y - 1) * surfaceWidth], surfaceWidth * sizeof(uint32_t));
    }

    // I/Q positive frequencies (0 Hz to Nyquist)
    // For I/Q signals, positive frequencies occupy bins 0..fftSize/2
    // At 96 kHz sampling: Nyquist = 48 kHz, so fftSize/2 = 1024 bins covering 0..48 kHz
    int visibleBins = fftSize / 8;  // For 2048 FFT: 1024 bins covering full positive spectrum
    int startBin = 0;  // Start from DC (0 Hz)
    
    LOGI("Spectrum: 0..%d Hz (positive only), visibleBins=%d, fftSize=%d, sampleRate=%d", currentSampleRate/2, visibleBins, fftSize, currentSampleRate);

    // Draw new line
    double minMag = 1e9, maxMag = -1e9;
    for (int x = 0; x < surfaceWidth; x++) {
        int binIdxInSpan = (x * visibleBins) / surfaceWidth;
        int binIdx = startBin + binIdxInSpan;
        
        // Clamp bin index to valid FFT range (0 to fftSize/2 - 1)
        int shiftedBin = binIdx;
        if (shiftedBin < 0) shiftedBin = 0;
        if (shiftedBin >= fftSize / 2) shiftedBin = fftSize / 2 - 1;

        double mag;
        
        if (monoModeEnabled) {
            // MONO MODE: Use only I (real) component magnitude
            mag = fabs(real[shiftedBin]);
        } else if (swapIQEnabled) {
            // SWAP I/Q: real = Q (from right), imag = I (from left)
            // Still use magnitude of complex pair
            mag = sqrt(real[shiftedBin] * real[shiftedBin] + imag[shiftedBin] * imag[shiftedBin]);
        } else if (stereoAverageEnabled) {
            // STEREO AVERAGE MODE: Average the magnitude of left and right channels
            // Since real[i] = left[i] and imag[i] = right[i]
            double leftMag = fabs(real[shiftedBin]);
            double rightMag = fabs(imag[shiftedBin]);
            mag = (leftMag + rightMag) / 2.0;  // Average amplitude
        } else {
            // I/Q magnitude (default): sqrt(I^2 + Q^2)
            mag = sqrt(real[shiftedBin] * real[shiftedBin] + imag[shiftedBin] * imag[shiftedBin]);
        }
        
        double logMag = 20 * log10(mag + 1e-6) + 120;
        
        if (logMag < minMag) minMag = logMag;
        if (logMag > maxMag) maxMag = logMag;
        
        waterfallBuffer[x] = getColor(logMag);
    }
    static int drawCount = 0;
    if (++drawCount % 10 == 0) {
        // Log first few FFT bins to see actual magnitude values
        const char* mode = monoModeEnabled ? "MONO" : (swapIQEnabled ? "I/Q_SWAP" : (stereoAverageEnabled ? "STEREO_AVG" : "I/Q"));
        LOGI("FFT Draw #%d [%s]: minMag=%.1f, maxMag=%.1f, range=%.1f, sampleRate=%d, bins: [%.1f, %.1f, %.1f, %.1f, %.1f]",
             drawCount, mode, minMag, maxMag, maxMag - minMag, currentSampleRate,
             20*log10(fabs(sqrt(real[0]*real[0]+imag[0]*imag[0]))+1e-6)+120,
             20*log10(fabs(sqrt(real[1]*real[1]+imag[1]*imag[1]))+1e-6)+120,
             20*log10(fabs(sqrt(real[256]*real[256]+imag[256]*imag[256]))+1e-6)+120,
             20*log10(fabs(sqrt(real[512]*real[512]+imag[512]*imag[512]))+1e-6)+120,
             20*log10(fabs(sqrt(real[768]*real[768]+imag[768]*imag[768]))+1e-6)+120);
    }

    ANativeWindow* window = ANativeWindow_fromSurface(env, surface);
    if (window) {
        ANativeWindow_Buffer buffer_out;
        if (ANativeWindow_lock(window, &buffer_out, nullptr) == 0) {
            if (buffer_out.width >= surfaceWidth && buffer_out.height >= surfaceHeight) {
                uint32_t* dest = (uint32_t*)buffer_out.bits;
                for (int y = 0; y < surfaceHeight; y++) {
                    memcpy(&dest[y * buffer_out.stride], &waterfallBuffer[y * surfaceWidth], surfaceWidth * sizeof(uint32_t));
                }
            }
            ANativeWindow_unlockAndPost(window);
        }
        ANativeWindow_release(window);
    }
}

// ============================================================================
// Oboe JNI Methods
// ============================================================================

// Audio capture thread function
void audioReadingThread() {
    if (g_audioCapture == nullptr || !g_isCapturing) return;

    JNIEnv* env = nullptr;
    bool attached = false;
    if (g_vm->GetEnv((void**)&env, JNI_VERSION_1_6) == JNI_EDETACHED) {
        g_vm->AttachCurrentThread(&env, nullptr);
        attached = true;
    }

    if (env == nullptr) {
        LOGI("ERROR: Failed to get JNI environment in audioReadingThread");
        return;
    }

    const int32_t fftSize = 2048;  // Match processAndDraw FFT size
    const int32_t framesPerRead = fftSize;
    int16_t* buffer = new int16_t[framesPerRead * 2]; // stereo

    jclass mainActivityClass = env->GetObjectClass(g_mainActivityRef);
    if (mainActivityClass == nullptr) {
        LOGI("ERROR: Failed to get mainActivityClass");
        delete[] buffer;
        if (attached) g_vm->DetachCurrentThread();
        return;
    }

    jmethodID processAndDrawMethod = env->GetMethodID(
        mainActivityClass, "processAndDraw", "([SILandroid/view/Surface;)V");

    if (processAndDrawMethod == nullptr) {
        LOGI("ERROR: Failed to find processAndDraw method");
        env->DeleteLocalRef(mainActivityClass);
        delete[] buffer;
        if (attached) g_vm->DetachCurrentThread();
        return;
    }

    LOGI("audioReadingThread: Started with mainActivityRef=%p, processAndDrawMethod=%p", g_mainActivityRef, processAndDrawMethod);

    int readCount = 0;
    int monoDetectCount = 0;
    int stereoDetectCount = 0;

    while (g_isCapturing && g_audioCapture != nullptr) {
        int32_t framesRead = g_audioCapture->readFrames(buffer, framesPerRead);
        if (framesRead < 0) {
            LOGI("AAudio read error");
            break;
        }
        if (framesRead == 0) {
            // No data available, sleep briefly
            usleep(1000);
            continue;
        }

        // Check if data is truly stereo or mono
        if (readCount % 10 == 0) {  // Log every 10 reads
            int64_t diffSum = 0;
            int16_t minVal = 32767, maxVal = -32768;
            int16_t left0 = 0, right0 = 0, left1 = 0, right1 = 0;
            
            for (int i = 0; i < framesRead && i < 5; i++) {
                int16_t left = buffer[2*i];
                int16_t right = buffer[2*i + 1];
                if (i == 0) { left0 = left; right0 = right; }
                if (i == 1) { left1 = left; right1 = right; }
                diffSum += abs(left - right);
                minVal = (left < minVal) ? left : minVal;
                maxVal = (left > maxVal) ? left : maxVal;
            }
            
            if (diffSum < 100) {
                monoDetectCount++;
                LOGI("Read %d: MONO (diffSum=%ld) L/R[0]=(%d,%d) L/R[1]=(%d,%d) range=[%d,%d]", 
                     readCount, diffSum, left0, right0, left1, right1, minVal, maxVal);
            } else {
                stereoDetectCount++;
                LOGI("Read %d: STEREO (diffSum=%ld) L/R[0]=(%d,%d) L/R[1]=(%d,%d)", 
                     readCount, diffSum, left0, right0, left1, right1);
            }
        }
        readCount++;

        // Create Java short array from buffer
        jshortArray javaBuffer = env->NewShortArray(framesRead * 2);
        if (javaBuffer == nullptr) {
            LOGI("ERROR: Failed to create short array for framesRead=%d", framesRead * 2);
            continue;
        }
        env->SetShortArrayRegion(javaBuffer, 0, framesRead * 2, buffer);

        // Call processAndDraw callback with the Surface reference (if available)
        jobject surface = g_surfaceRef;  // Use the stored surface reference
        if (readCount % 100 == 0) {
            LOGI("Calling processAndDraw: framesRead=%d, surface=%p, mainActivityRef=%p", framesRead * 2, surface, g_mainActivityRef);
        }
        
        env->CallVoidMethod(g_mainActivityRef, processAndDrawMethod, javaBuffer, framesRead * 2, surface);
        
        // Check for JNI exceptions
        if (env->ExceptionCheck()) {
            LOGI("ERROR: Exception in CallVoidMethod");
            env->ExceptionDescribe();
            env->ExceptionClear();
        }
        
        env->DeleteLocalRef(javaBuffer);
    }

    delete[] buffer;
    if (attached) g_vm->DetachCurrentThread();
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_example_belkarx_MainActivity_startOboeCapture(JNIEnv* env, jobject thisObj, jint deviceId, jint sampleRate) {
    if (g_audioCapture != nullptr) {
        LOGI("Oboe capture already running");
        return JNI_FALSE;
    }

    // Save VM and Activity reference for callbacks
    env->GetJavaVM(&g_vm);
    g_mainActivityRef = env->NewGlobalRef(thisObj);

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
        return JNI_FALSE;
    }

    g_isCapturing = true;
    
    // Spawn audio reading thread
    g_audioThread = new std::thread(audioReadingThread);

    LOGI("Oboe capture started successfully");
    return JNI_TRUE;
}

extern "C" JNIEXPORT void JNICALL
Java_com_example_belkarx_MainActivity_stopOboeCapture(JNIEnv* env, jobject /* this */) {
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

    // Clean up Java references
    if (g_mainActivityRef != nullptr) {
        env->DeleteGlobalRef(g_mainActivityRef);
        g_mainActivityRef = nullptr;
    }

    LOGI("Oboe capture stopped");
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_example_belkarx_MainActivity_isOboeRunning(JNIEnv* env, jobject /* this */) {
    if (g_audioCapture == nullptr) return JNI_FALSE;
    return g_audioCapture->isRunning() ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT jint JNICALL
Java_com_example_belkarx_MainActivity_getOboeChannels(JNIEnv* env, jobject /* this */) {
    if (g_audioCapture == nullptr) return 0;
    return g_audioCapture->getChannelCount();
}

extern "C" JNIEXPORT jint JNICALL
Java_com_example_belkarx_MainActivity_getOboeSampleRate(JNIEnv* env, jobject /* this */) {
    if (g_audioCapture == nullptr) return 0;
    return g_audioCapture->getSampleRate();
}

extern "C" JNIEXPORT void JNICALL
Java_com_example_belkarx_MainActivity_setOboeSurface(JNIEnv* env, jobject /* this */, jobject surface) {
    // Clean up old surface reference if it exists
    if (g_surfaceRef != nullptr) {
        env->DeleteGlobalRef(g_surfaceRef);
        LOGI("setOboeSurface: Deleted old surface reference");
    }
    
    // Store new surface reference
    if (surface != nullptr) {
        g_surfaceRef = env->NewGlobalRef(surface);
        LOGI("setOboeSurface: Updated to new surface reference %p", surface);
    } else {
        g_surfaceRef = nullptr;
        LOGI("setOboeSurface: Cleared surface reference");
    }
}
