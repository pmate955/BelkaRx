#pragma once

#include <cstdint>

#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <android/native_window.h>

// OpenGL ES 3 presenter for the waterfall. Owned and used by the render thread only.
//
// The waterfall history lives in a texture with one texel row per FFT line (the same ring
// layout as WaterfallRing: line n in row n % capacity). The fragment shader samples it at a
// fractional row position with linear filtering, so the picture scrolls by sub-pixel amounts
// every frame instead of jumping whole rows. Markers, the frequency scale and the spectrum view
// are drawn on the CPU into an overlay texture that is blended on top.
class GlWaterfall {
public:
    // Makes the context current on `window`. A new `generation` recreates the EGL surface.
    // Returns false if EGL/GL is unavailable. outWidth/outHeight receive the surface size.
    bool makeCurrent(ANativeWindow* window, int generation, int& outWidth, int& outHeight);

    // Recreates the waterfall texture when the ring changes (new size), filled from `pixels`.
    // Returns true if the texture was (re)created.
    bool ensureWaterfallTexture(const void* ringId, int width, int capacityRows, const uint32_t* pixels);

    // Uploads `count` consecutive ring rows starting at `firstSlot` (no wrap inside the call).
    void uploadWaterfallRows(int firstSlot, int count, const uint32_t* pixels);

    // Uploads a full-surface RGBA overlay (row 0 is the top of the screen).
    void uploadOverlay(int width, int height, const uint32_t* pixels);

    // Draws the frame. topRow is the fractional ring row shown at the top of the waterfall,
    // maxAgeRows how many rows below it hold valid data (negative: no waterfall at all).
    void draw(int surfaceWidth, int surfaceHeight, int topMargin, double topRow, double maxAgeRows);

    bool present();

    // Destroys the EGL surface (keeps the context). Call before the window goes away.
    void releaseSurface();
    // Destroys everything.
    void releaseAll();

    int waterfallWidth() const { return wfWidth_; }
    int waterfallCapacity() const { return wfCapacity_; }

private:
    bool initDisplayAndContext();
    bool initProgram();

    EGLDisplay display_ = EGL_NO_DISPLAY;
    EGLConfig config_ = nullptr;
    EGLContext context_ = EGL_NO_CONTEXT;
    EGLSurface surface_ = EGL_NO_SURFACE;
    int surfaceGeneration_ = -1;
    bool failed_ = false;

    GLuint program_ = 0;
    GLuint vao_ = 0;
    GLint uSurfaceSize_ = -1, uTopMargin_ = -1, uCapacity_ = -1, uTopRow_ = -1, uMaxAge_ = -1;
    GLint uWaterfall_ = -1, uOverlay_ = -1;

    GLuint wfTexture_ = 0;
    const void* wfRingId_ = nullptr;
    int wfWidth_ = 0;
    int wfCapacity_ = 0;

    GLuint overlayTexture_ = 0;
    int overlayWidth_ = 0;
    int overlayHeight_ = 0;
};
