#pragma once

#include <jni.h>

bool startOboeCaptureImpl(JNIEnv* env, jobject activity, jint deviceId, jint sampleRate);
void stopOboeCaptureImpl(JNIEnv* env);
void setOboeSurfaceImpl(JNIEnv* env, jobject surface);
