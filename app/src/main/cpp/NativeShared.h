#pragma once

#include <jni.h>
#include <mutex>

#include <android/native_window.h>

extern std::mutex g_mutex;
extern int currentSampleRate;
extern bool showSpectrumEnabled;
extern ANativeWindow* g_nativeWindow;
