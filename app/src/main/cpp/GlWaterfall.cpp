#include "GlWaterfall.h"

#include <cmath>

#include <EGL/eglext.h>
#include <android/log.h>

#define GL_TAG "BelkaRx-GL"
#define GLOGI(...) __android_log_print(ANDROID_LOG_INFO, GL_TAG, __VA_ARGS__)
#define GLOGE(...) __android_log_print(ANDROID_LOG_ERROR, GL_TAG, __VA_ARGS__)

namespace {

// Full-screen triangle generated from gl_VertexID, no vertex buffers needed.
const char* kVertexShader = R"(#version 300 es
void main() {
    vec2 p = vec2(gl_VertexID == 1 ? 3.0 : -1.0, gl_VertexID == 2 ? 3.0 : -1.0);
    gl_Position = vec4(p, 0.0, 1.0);
}
)";

// Vertical sampling uses a cubic B-spline over 4 ring rows instead of linear blending.
// Linear blending is sharp when the scroll sits exactly on a row and averages two rows when it
// sits halfway, so the noise texture pulses ~94 times a second ("vibration"). The B-spline
// smooths almost equally at every sub-row offset, so the picture keeps the same look while it
// moves. It needs one row on each side of the blended pair: the renderer stays at least one row
// behind the newest one and stops one row before the oldest valid row.
const char* kFragmentShader = R"(#version 300 es
precision highp float;
precision highp int;
uniform highp sampler2D uWaterfall;
uniform sampler2D uOverlay;
uniform vec2 uSurfaceSize;
uniform float uTopMargin;
uniform float uCapacity;
uniform float uTopRow;   // fractional ring row at the top of the waterfall, modulo capacity
uniform float uMaxAge;   // rows below the top that hold valid data
out vec4 outColor;

vec3 ringRow(int ix, float row) {
    return texelFetch(uWaterfall, ivec2(ix, int(mod(row, uCapacity))), 0).rgb;
}

void main() {
    float fromTop = uSurfaceSize.y - gl_FragCoord.y;  // pixel center, measured from the top
    float x = gl_FragCoord.x / uSurfaceSize.x;
    vec3 base = vec3(0.0);
    float age = fromTop - 0.5 - uTopMargin;            // 0 on the first waterfall row
    if (age >= 0.0 && age <= uMaxAge) {
        int texWidth = textureSize(uWaterfall, 0).x;
        int ix = clamp(int(x * float(texWidth)), 0, texWidth - 1);
        float r = uTopRow - age;
        float r0 = floor(r);
        float f = r - r0;
        float f2 = f * f;
        float f3 = f2 * f;
        float w0 = (1.0 - f) * (1.0 - f) * (1.0 - f) / 6.0;
        float w1 = (3.0 * f3 - 6.0 * f2 + 4.0) / 6.0;
        float w2 = (-3.0 * f3 + 3.0 * f2 + 3.0 * f + 1.0) / 6.0;
        float w3 = f3 / 6.0;
        base = w0 * ringRow(ix, r0 - 1.0) + w1 * ringRow(ix, r0) +
               w2 * ringRow(ix, r0 + 1.0) + w3 * ringRow(ix, r0 + 2.0);
    }
    vec4 overlay = texture(uOverlay, vec2(x, fromTop / uSurfaceSize.y));
    outColor = vec4(mix(base, overlay.rgb, overlay.a), 1.0);
}
)";

GLuint compileShader(GLenum type, const char* source) {
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);
    GLint ok = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024];
        glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
        GLOGE("shader compile failed: %s", log);
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

void setupTextureParams(bool repeatVertically) {
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, repeatVertically ? GL_REPEAT : GL_CLAMP_TO_EDGE);
}

}  // namespace

bool GlWaterfall::initDisplayAndContext() {
    if (display_ != EGL_NO_DISPLAY) return true;

    display_ = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (display_ == EGL_NO_DISPLAY || !eglInitialize(display_, nullptr, nullptr)) {
        GLOGE("eglInitialize failed");
        display_ = EGL_NO_DISPLAY;
        return false;
    }

    const EGLint configAttribs[] = {
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT_KHR,
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
        EGL_DEPTH_SIZE, 0, EGL_STENCIL_SIZE, 0,
        EGL_NONE};
    EGLint numConfigs = 0;
    if (!eglChooseConfig(display_, configAttribs, &config_, 1, &numConfigs) || numConfigs < 1) {
        GLOGE("eglChooseConfig failed");
        return false;
    }

    const EGLint contextAttribs[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
    context_ = eglCreateContext(display_, config_, EGL_NO_CONTEXT, contextAttribs);
    if (context_ == EGL_NO_CONTEXT) {
        GLOGE("eglCreateContext failed: 0x%x", eglGetError());
        return false;
    }
    return true;
}

bool GlWaterfall::initProgram() {
    if (program_ != 0) return true;

    GLuint vs = compileShader(GL_VERTEX_SHADER, kVertexShader);
    GLuint fs = compileShader(GL_FRAGMENT_SHADER, kFragmentShader);
    if (!vs || !fs) return false;

    program_ = glCreateProgram();
    glAttachShader(program_, vs);
    glAttachShader(program_, fs);
    glLinkProgram(program_);
    glDeleteShader(vs);
    glDeleteShader(fs);
    GLint ok = 0;
    glGetProgramiv(program_, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[1024];
        glGetProgramInfoLog(program_, sizeof(log), nullptr, log);
        GLOGE("program link failed: %s", log);
        glDeleteProgram(program_);
        program_ = 0;
        return false;
    }

    uSurfaceSize_ = glGetUniformLocation(program_, "uSurfaceSize");
    uTopMargin_ = glGetUniformLocation(program_, "uTopMargin");
    uCapacity_ = glGetUniformLocation(program_, "uCapacity");
    uTopRow_ = glGetUniformLocation(program_, "uTopRow");
    uMaxAge_ = glGetUniformLocation(program_, "uMaxAge");
    uWaterfall_ = glGetUniformLocation(program_, "uWaterfall");
    uOverlay_ = glGetUniformLocation(program_, "uOverlay");

    glGenVertexArrays(1, &vao_);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);

    GLint maxTexture = 0;
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maxTexture);
    GLOGI("GL ready: %s / %s, max texture %d", glGetString(GL_RENDERER), glGetString(GL_VERSION), maxTexture);
    return true;
}

bool GlWaterfall::makeCurrent(ANativeWindow* window, int generation, int& outWidth, int& outHeight) {
    if (failed_ || window == nullptr) return false;
    if (!initDisplayAndContext()) {
        failed_ = true;
        return false;
    }

    if (surface_ == EGL_NO_SURFACE || surfaceGeneration_ != generation) {
        releaseSurface();
        surface_ = eglCreateWindowSurface(display_, config_, window, nullptr);
        if (surface_ == EGL_NO_SURFACE) {
            GLOGE("eglCreateWindowSurface failed: 0x%x", eglGetError());
            return false;
        }
        surfaceGeneration_ = generation;
        if (!eglMakeCurrent(display_, surface_, surface_, context_)) {
            GLOGE("eglMakeCurrent failed: 0x%x", eglGetError());
            releaseSurface();
            return false;
        }
        eglSwapInterval(display_, 1);
        if (!initProgram()) {
            failed_ = true;
            return false;
        }
    }

    EGLint w = 0, h = 0;
    eglQuerySurface(display_, surface_, EGL_WIDTH, &w);
    eglQuerySurface(display_, surface_, EGL_HEIGHT, &h);
    outWidth = w;
    outHeight = h;
    return w > 0 && h > 0;
}

bool GlWaterfall::ensureWaterfallTexture(const void* ringId, int width, int capacityRows, const uint32_t* pixels) {
    if (wfTexture_ != 0 && ringId == wfRingId_ && width == wfWidth_ && capacityRows == wfCapacity_) return false;

    if (wfTexture_ == 0) glGenTextures(1, &wfTexture_);
    glBindTexture(GL_TEXTURE_2D, wfTexture_);
    setupTextureParams(true);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, capacityRows, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    wfRingId_ = ringId;
    wfWidth_ = width;
    wfCapacity_ = capacityRows;
    return true;
}

void GlWaterfall::uploadWaterfallRows(int firstSlot, int count, const uint32_t* pixels) {
    if (wfTexture_ == 0 || count <= 0) return;
    glBindTexture(GL_TEXTURE_2D, wfTexture_);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, firstSlot, wfWidth_, count, GL_RGBA, GL_UNSIGNED_BYTE,
                    pixels + static_cast<size_t>(firstSlot) * wfWidth_);
}

void GlWaterfall::uploadOverlay(int width, int height, const uint32_t* pixels) {
    if (overlayTexture_ == 0) glGenTextures(1, &overlayTexture_);
    glBindTexture(GL_TEXTURE_2D, overlayTexture_);
    if (width != overlayWidth_ || height != overlayHeight_) {
        setupTextureParams(false);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
        overlayWidth_ = width;
        overlayHeight_ = height;
    } else {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    }
}

void GlWaterfall::draw(int surfaceWidth, int surfaceHeight, int topMargin, double topRow, double maxAgeRows) {
    glViewport(0, 0, surfaceWidth, surfaceHeight);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    if (program_ == 0 || wfTexture_ == 0 || overlayTexture_ == 0) return;

    double capacity = wfCapacity_ > 0 ? wfCapacity_ : 1;
    double topRowMod = std::fmod(topRow, capacity);
    if (topRowMod < 0.0) topRowMod += capacity;

    glUseProgram(program_);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, wfTexture_);
    glUniform1i(uWaterfall_, 0);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, overlayTexture_);
    glUniform1i(uOverlay_, 1);
    glActiveTexture(GL_TEXTURE0);

    glUniform2f(uSurfaceSize_, static_cast<float>(surfaceWidth), static_cast<float>(surfaceHeight));
    glUniform1f(uTopMargin_, static_cast<float>(topMargin));
    glUniform1f(uCapacity_, static_cast<float>(capacity));
    glUniform1f(uTopRow_, static_cast<float>(topRowMod));
    glUniform1f(uMaxAge_, static_cast<float>(maxAgeRows));

    glBindVertexArray(vao_);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindVertexArray(0);
}

bool GlWaterfall::present() {
    if (surface_ == EGL_NO_SURFACE) return false;
    if (!eglSwapBuffers(display_, surface_)) {
        EGLint err = eglGetError();
        GLOGE("eglSwapBuffers failed: 0x%x", err);
        if (err == EGL_BAD_SURFACE || err == EGL_BAD_NATIVE_WINDOW) releaseSurface();
        return false;
    }
    return true;
}

void GlWaterfall::releaseSurface() {
    if (display_ == EGL_NO_DISPLAY) return;
    if (surface_ != EGL_NO_SURFACE) {
        // Keep the context (and its textures) alive by making it current without a surface.
        eglMakeCurrent(display_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        eglDestroySurface(display_, surface_);
        surface_ = EGL_NO_SURFACE;
    }
    surfaceGeneration_ = -1;
}

void GlWaterfall::releaseAll() {
    if (display_ == EGL_NO_DISPLAY) return;
    if (context_ != EGL_NO_CONTEXT && surface_ != EGL_NO_SURFACE) {
        eglMakeCurrent(display_, surface_, surface_, context_);
        if (wfTexture_) glDeleteTextures(1, &wfTexture_);
        if (overlayTexture_) glDeleteTextures(1, &overlayTexture_);
        if (program_) glDeleteProgram(program_);
        if (vao_) glDeleteVertexArrays(1, &vao_);
    }
    releaseSurface();
    if (context_ != EGL_NO_CONTEXT) eglDestroyContext(display_, context_);
    eglTerminate(display_);
    display_ = EGL_NO_DISPLAY;
    context_ = EGL_NO_CONTEXT;
    config_ = nullptr;
    wfTexture_ = overlayTexture_ = program_ = vao_ = 0;
    wfRingId_ = nullptr;
    wfWidth_ = wfCapacity_ = overlayWidth_ = overlayHeight_ = 0;
    failed_ = false;
}
