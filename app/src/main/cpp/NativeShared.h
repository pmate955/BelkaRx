#pragma once

#include <jni.h>
#include <mutex>
#include <thread>

#include <android/native_window.h>

#include "OboeCapture.h"

extern std::mutex g_mutex;
extern int currentSampleRate;
extern bool showSpectrumEnabled;
extern jobject g_surfaceRef;
extern JavaVM* g_vm;
extern jobject g_mainActivityRef;
extern ANativeWindow* g_nativeWindow;
extern OboeCapture* g_audioCapture;
extern std::thread* g_audioThread;
extern bool g_isCapturing;

extern "C" JNIEXPORT void JNICALL
Java_com_example_belkarx_MainActivity_processAndDraw(JNIEnv* env, jobject thiz, jshortArray data, jint size, jobject surface);
