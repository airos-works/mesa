/*
 * MIT License
 *
 * Copyright (c) 2026 KunanyiOS contributors
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 * The engine/patches directory contains Haiku WebKit source changes under their
 * original per-file licenses, including BSD and LGPL. This license does not
 * replace those notices. The vendored JSON library has its own MIT notice.
 */
// Adapted from Summit c74554e520956da32b366247b814b7bb3be14c0f
// tools/mesa-vm/gles2-draw-probe.cpp; native renderer/IRQ checks added.
// GLES2 compositor probe for the private native-enabled Mesa stack.
// It exercises what a TextureMapper-style compositor needs from GL:
//   * a surfaceless context (EGL_KHR_surfaceless_context) and a pbuffer context
//   * a second context sharing textures with the first
//   * GLSL ES 1.00 shader compile/link, VBO draw of a textured quad into an FBO
//   * alpha blending, scissor, and exact readback of every pixel
//   * frame timing for a 1024x768 textured, blended full-screen pass
//   * the same scene on a secondary thread (compositor threads are not main)
//   * a display from eglGetPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA)
// Exit status 0 only when every check passes.
//
// Build with private EGL/GLES2 headers and libraries, -mtls-dialect=gnu,
// and -lEGL -lGLESv2 -lbe. Include this directory for the native ABI headers.
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <OS.h>
#include <pthread.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "amdgpu_haiku.h"
#include <errno.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>

static void require(bool ok, const char *what)
{
   if (ok) return;
   fprintf(stderr, "FAIL: %s EGL %#x GL %#x errno %d\n", what, eglGetError(), glGetError(), errno);
   exit(1);
}
static amdgpu_irq_info interrupts(int fd)
{
   amdgpu_irq_info info = {};
   info.version = AMDGPU_HAIKU_ABI_VERSION; info.size = sizeof(info);
   require(ioctl(fd, AMDGPU_IRQ_INFO, &info, sizeof(info)) == 0, "native IRQ query");
   require(info.enabled && info.msi && info.status == B_OK && !info.vm_faults
           && !info.privileged_faults && !info.overflows && !info.unknown, "healthy native IRQs");
   return info;
}

static int failures = 0;

#define CHECK(cond, ...) \
    do { \
        if (!(cond)) { \
            ++failures; \
            printf("FAIL %s:%d: ", __FILE__, __LINE__); \
            printf(__VA_ARGS__); \
            printf("\n"); \
        } \
    } while (0)

static GLuint compile(GLenum type, const char* source)
{
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);
    GLint ok = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024] = {};
        glGetShaderInfoLog(shader, sizeof(log) - 1, nullptr, log);
        CHECK(false, "shader compile: %s", log);
    }
    return shader;
}

static GLuint linkProgram(const char* vertex, const char* fragment)
{
    GLuint program = glCreateProgram();
    GLuint vs = compile(GL_VERTEX_SHADER, vertex);
    GLuint fs = compile(GL_FRAGMENT_SHADER, fragment);
    glAttachShader(program, vs);
    glAttachShader(program, fs);
    glBindAttribLocation(program, 0, "a_position");
    glBindAttribLocation(program, 1, "a_texcoord");
    glLinkProgram(program);
    GLint ok = 0;
    glGetProgramiv(program, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[1024] = {};
        glGetProgramInfoLog(program, sizeof(log) - 1, nullptr, log);
        CHECK(false, "program link: %s", log);
    }
    glDeleteShader(vs);
    glDeleteShader(fs);
    return program;
}

static const char* kVertex =
    "attribute vec2 a_position;\n"
    "attribute vec2 a_texcoord;\n"
    "uniform vec4 u_rect;\n" // x, y, w, h in clip space
    "varying vec2 v_texcoord;\n"
    "void main() {\n"
    "  v_texcoord = a_texcoord;\n"
    "  gl_Position = vec4(u_rect.xy + a_position * u_rect.zw, 0.0, 1.0);\n"
    "}\n";

static const char* kFragment =
    "precision mediump float;\n"
    "uniform sampler2D u_texture;\n"
    "uniform float u_opacity;\n"
    "varying vec2 v_texcoord;\n"
    "void main() {\n"
    "  gl_FragColor = texture2D(u_texture, v_texcoord) * u_opacity;\n"
    "}\n";

struct Target {
    GLuint texture = 0;
    GLuint framebuffer = 0;
    int width = 0, height = 0;
};

static Target makeTarget(int width, int height)
{
    Target target;
    target.width = width;
    target.height = height;
    glGenTextures(1, &target.texture);
    glBindTexture(GL_TEXTURE_2D, target.texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glGenFramebuffers(1, &target.framebuffer);
    glBindFramebuffer(GL_FRAMEBUFFER, target.framebuffer);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, target.texture, 0);
    CHECK(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE, "fbo incomplete");
    return target;
}

static bool near(int a, int b) { return abs(a - b) <= 1; }

static int runScene(const char* label, EGLDisplay display, EGLContext context, EGLContext uploader,
    EGLSurface surface)
{
    // 1. The sharing context uploads a 2x2 tile texture (red, green / blue, white).
    CHECK(eglMakeCurrent(display, surface, surface, uploader), "%s: uploader current 0x%x", label, eglGetError());
    GLuint tile = 0;
    glGenTextures(1, &tile);
    glBindTexture(GL_TEXTURE_2D, tile);
    const unsigned char texels[16] = {
        255, 0, 0, 255, 0, 255, 0, 255,
        0, 0, 255, 255, 255, 255, 255, 255 };
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 2, 2, 0, GL_RGBA, GL_UNSIGNED_BYTE, texels);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glFinish();

    // 2. The compositor context draws with the shared texture.
    CHECK(eglMakeCurrent(display, surface, surface, context), "%s: compositor current 0x%x", label, eglGetError());
    CHECK(glIsTexture(tile), "%s: shared texture not visible", label);
    GLuint program = linkProgram(kVertex, kFragment);
    glUseProgram(program);
    GLint rect = glGetUniformLocation(program, "u_rect");
    GLint opacity = glGetUniformLocation(program, "u_opacity");
    glUniform1i(glGetUniformLocation(program, "u_texture"), 0);

    const float quad[] = { 0, 0, 0, 0, 1, 0, 1, 0, 0, 1, 0, 1, 1, 1, 1, 1 };
    GLuint buffer = 0;
    glGenBuffers(1, &buffer);
    glBindBuffer(GL_ARRAY_BUFFER, buffer);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 16, (void*)0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 16, (void*)8);

    const int W = 64, H = 64;
    Target target = makeTarget(W, H);
    glViewport(0, 0, W, H);
    glDisable(GL_BLEND);
    glDisable(GL_SCISSOR_TEST);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tile);
    // Opaque full-target tile.
    glUniform4f(rect, -1, -1, 2, 2);
    glUniform1f(opacity, 1.0f);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    // Premultiplied 50% white layer over the right half, clipped by scissor to the top half.
    const unsigned char white[4] = { 255, 255, 255, 255 };
    GLuint layer = 0;
    glGenTextures(1, &layer);
    glBindTexture(GL_TEXTURE_2D, layer);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, white);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glEnable(GL_SCISSOR_TEST);
    glScissor(0, H / 2, W, H / 2);
    glUniform4f(rect, 0, -1, 1, 2);
    glUniform1f(opacity, 0.5f);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_BLEND);

    std::vector<unsigned char> pixels(W * H * 4);
    glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
    CHECK(glGetError() == GL_NO_ERROR, "%s: GL error after scene", label);
    int bad = 0;
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            // Texture row 0 is the bottom half of the target. The 50% white
            // layer only reaches the top-right quadrant, which is already
            // white; a missing scissor would turn bottom-right green into
            // 128,255,128.
            int expected[3];
            bool right = x >= W / 2, top = y >= H / 2;
            if (!top && !right) { expected[0] = 255; expected[1] = 0; expected[2] = 0; }
            else if (!top && right) { expected[0] = 0; expected[1] = 255; expected[2] = 0; }
            else if (top && !right) { expected[0] = 0; expected[1] = 0; expected[2] = 255; }
            else { expected[0] = 255; expected[1] = 255; expected[2] = 255; }
            const unsigned char* p = &pixels[(y * W + x) * 4];
            if (!near(p[0], expected[0]) || !near(p[1], expected[1]) || !near(p[2], expected[2]) || p[3] != 255) {
                if (bad++ < 4)
                    printf("  %s: pixel %d,%d = %u,%u,%u,%u expected %d,%d,%d,255\n", label, x, y,
                        p[0], p[1], p[2], p[3], expected[0], expected[1], expected[2]);
            }
        }
    }
    // Blend check on a non-white base: draw the 50% layer over the bottom-left red quadrant.
    glBindFramebuffer(GL_FRAMEBUFFER, target.framebuffer);
    glEnable(GL_BLEND);
    glBindTexture(GL_TEXTURE_2D, layer);
    glUniform4f(rect, -1, -1, 1, 1);
    glUniform1f(opacity, 0.5f);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glDisable(GL_BLEND);
    unsigned char blended[4] = {};
    glReadPixels(3, 3, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, blended);
    CHECK(near(blended[0], 255) && near(blended[1], 128) && near(blended[2], 128) && blended[3] == 255,
        "%s: blend result %u,%u,%u,%u expected 255,128,128,255", label, blended[0], blended[1], blended[2], blended[3]);
    CHECK(bad == 0, "%s: %d of %d scene pixels wrong", label, bad, W * H);
    printf("%s: scene %dx%d checked, %d bad pixels, blend=%u,%u,%u,%u\n", label, W, H, bad,
        blended[0], blended[1], blended[2], blended[3]);

    // 3. Timing: 1024x768 blended textured full-target pass + finish, and with readback.
    const int BW = 1024, BH = 768, frames = 30;
    Target big = makeTarget(BW, BH);
    glViewport(0, 0, BW, BH);
    glEnable(GL_BLEND);
    glBindTexture(GL_TEXTURE_2D, tile);
    glUniform4f(rect, -1, -1, 2, 2);
    glUniform1f(opacity, 0.75f);
    bigtime_t start = system_time();
    for (int i = 0; i < frames; ++i) {
        glClear(GL_COLOR_BUFFER_BIT);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        glFinish();
    }
    double drawMs = (system_time() - start) / 1000.0 / frames;
    std::vector<unsigned char> bigPixels(BW * BH * 4);
    start = system_time();
    for (int i = 0; i < frames; ++i) {
        glClear(GL_COLOR_BUFFER_BIT);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        glReadPixels(0, 0, BW, BH, GL_RGBA, GL_UNSIGNED_BYTE, bigPixels.data());
    }
    double readMs = (system_time() - start) / 1000.0 / frames;
    glDisable(GL_BLEND);
    CHECK(glGetError() == GL_NO_ERROR, "%s: GL error after timing", label);
    int bigBad = 0;
    for (int y = 0; y < BH; ++y) {
        for (int x = 0; x < BW; ++x) {
            const unsigned char* texel = texels + ((y >= BH / 2) * 2 + (x >= BW / 2)) * 4;
            const unsigned char* pixel = bigPixels.data() + (y * BW + x) * 4;
            bool correct = pixel[3] == 255;
            for (int channel = 0; channel < 3; ++channel)
                correct = correct && near(pixel[channel], texel[channel] ? 191 : 0);
            bigBad += !correct;
        }
    }
    CHECK(bigBad == 0, "%s: %d of %d large scene pixels wrong", label, bigBad, BW * BH);
    printf("%s: large scene %dx%d checked, %d bad pixels\n", label, BW, BH, bigBad);
    printf("%s: %dx%d blended textured quad: %.2f ms/frame draw+finish, %.2f ms/frame draw+readback\n",
        label, BW, BH, drawMs, readMs);

    glDeleteFramebuffers(1, &big.framebuffer);
    glDeleteTextures(1, &big.texture);
    glDeleteFramebuffers(1, &target.framebuffer);
    glDeleteTextures(1, &target.texture);
    glDeleteTextures(1, &layer);
    glDeleteTextures(1, &tile);
    glDeleteBuffers(1, &buffer);
    glDeleteProgram(program);
    return 0;
}

struct ThreadArgs {
    EGLDisplay display;
    EGLConfig config;
    EGLContext share;
};

// A compositor thread: its own context (sharing with the main one), made
// current without any surface, never touching the main thread's binding.
static void* threadMain(void* data)
{
    ThreadArgs* args = static_cast<ThreadArgs*>(data);
    CHECK(eglBindAPI(EGL_OPENGL_ES_API), "thread: bind api");
    EGLint contextAttrs[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    EGLContext context = eglCreateContext(args->display, args->config, args->share, contextAttrs);
    EGLContext uploader = eglCreateContext(args->display, args->config, context, contextAttrs);
    CHECK(context != EGL_NO_CONTEXT && uploader != EGL_NO_CONTEXT, "thread: context creation 0x%x", eglGetError());
    if (context != EGL_NO_CONTEXT && uploader != EGL_NO_CONTEXT)
        runScene("thread", args->display, context, uploader, EGL_NO_SURFACE);
    eglMakeCurrent(args->display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroyContext(args->display, uploader);
    eglDestroyContext(args->display, context);
    eglReleaseThread();
    return nullptr;
}

#ifndef EGL_PLATFORM_SURFACELESS_MESA
#define EGL_PLATFORM_SURFACELESS_MESA 0x31DD
#endif

static void surfacelessPlatform()
{
    const char* client = eglQueryString(EGL_NO_DISPLAY, EGL_EXTENSIONS);
    if (!client || !strstr(client, "EGL_MESA_platform_surfaceless")) {
        printf("platform-surfaceless: EGL_MESA_platform_surfaceless not advertised\n");
        return;
    }
    EGLDisplay display = eglGetPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, nullptr);
    EGLint major = 0, minor = 0;
    if (display == EGL_NO_DISPLAY || !eglInitialize(display, &major, &minor)) {
        CHECK(false, "platform-surfaceless: display/initialize 0x%x", eglGetError());
        return;
    }
    EGLint attrs[] = { EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_NONE };
    EGLConfig config;
    EGLint count = 0;
    CHECK(eglChooseConfig(display, attrs, &config, 1, &count) && count > 0, "platform-surfaceless: config");
    EGLint contextAttrs[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    EGLContext context = eglCreateContext(display, count ? config : nullptr, EGL_NO_CONTEXT, contextAttrs);
    CHECK(context != EGL_NO_CONTEXT, "platform-surfaceless: context 0x%x", eglGetError());
    if (context != EGL_NO_CONTEXT && eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, context)) {
        Target target = makeTarget(8, 8);
        glViewport(0, 0, 8, 8);
        glClearColor(0.0f, 1.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        unsigned char p[4] = {};
        glReadPixels(4, 4, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, p);
        CHECK(p[0] == 0 && p[1] == 255 && p[2] == 0 && p[3] == 255, "platform-surfaceless: pixel %u,%u,%u,%u", p[0], p[1], p[2], p[3]);
        printf("platform-surfaceless: EGL %d.%d renderer=%s pixel=%u,%u,%u,%u\n", major, minor,
            glGetString(GL_RENDERER), p[0], p[1], p[2], p[3]);
        glDeleteFramebuffers(1, &target.framebuffer);
        glDeleteTextures(1, &target.texture);
    } else
        CHECK(false, "platform-surfaceless: makeCurrent 0x%x", eglGetError());
    eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    if (context != EGL_NO_CONTEXT)
        eglDestroyContext(display, context);
    eglTerminate(display);
}

int main()
{
    // Unbuffered, so a run that hangs still shows how far it got when its
    // output is a pipe or a file.
    setvbuf(stdout, nullptr, _IONBF, 0);
    disable_debugger(1);
    bool hardware = getenv("HAIKU_AMDGPU_DEVICE") != nullptr;
    int observer = hardware ? open(getenv("HAIKU_AMDGPU_DEVICE"), O_RDWR) : -1;
    if (hardware) require(observer >= 0, "open independent GPU observer");
    amdgpu_irq_info before = hardware ? interrupts(observer) : amdgpu_irq_info{};
    EGLDisplay display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    EGLint major = 0, minor = 0;
    if (!eglInitialize(display, &major, &minor)) {
        printf("FAIL eglInitialize 0x%x\n", eglGetError());
        return 1;
    }
    printf("EGL %d.%d vendor=%s version=%s\n", major, minor, eglQueryString(display, EGL_VENDOR),
        eglQueryString(display, EGL_VERSION));
    const char* extensions = eglQueryString(display, EGL_EXTENSIONS);
    bool surfaceless = extensions && strstr(extensions, "EGL_KHR_surfaceless_context");
    CHECK(eglBindAPI(EGL_OPENGL_ES_API), "bind api");
    EGLint attrs[] = { EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE };
    EGLConfig config;
    EGLint count = 0;
    if (!eglChooseConfig(display, attrs, &config, 1, &count) || count < 1) {
        printf("FAIL no pbuffer ES2 config 0x%x\n", eglGetError());
        return 1;
    }
    EGLint contextAttrs[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    EGLContext context = eglCreateContext(display, config, EGL_NO_CONTEXT, contextAttrs);
    EGLContext uploader = eglCreateContext(display, config, context, contextAttrs);
    CHECK(context != EGL_NO_CONTEXT && uploader != EGL_NO_CONTEXT, "context creation 0x%x", eglGetError());

    if (surfaceless) {
        if (eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, context)) {
            printf("GL_RENDERER=%s\nGL_VERSION=%s\n", glGetString(GL_RENDERER), glGetString(GL_VERSION));
            if (hardware)
                CHECK(strstr((const char*)glGetString(GL_RENDERER), "polaris10"), "expected native Polaris renderer");
            runScene("surfaceless", display, context, uploader, EGL_NO_SURFACE);
        } else
            CHECK(false, "surfaceless makeCurrent 0x%x", eglGetError());
    } else
        printf("surfaceless: EGL_KHR_surfaceless_context not advertised\n");

    EGLint pbufferAttrs[] = { EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE };
    EGLSurface pbuffer = eglCreatePbufferSurface(display, config, pbufferAttrs);
    CHECK(pbuffer != EGL_NO_SURFACE, "pbuffer 0x%x", eglGetError());
    if (pbuffer != EGL_NO_SURFACE) {
        runScene("pbuffer", display, context, uploader, pbuffer);
        // Default framebuffer of the pbuffer itself.
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glViewport(0, 0, 16, 16);
        glClearColor(1.0f, 0.5f, 0.25f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        unsigned char p[4] = {};
        glReadPixels(8, 8, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, p);
        CHECK(near(p[0], 255) && near(p[1], 128) && near(p[2], 64) && p[3] == 255,
            "pbuffer default framebuffer %u,%u,%u,%u", p[0], p[1], p[2], p[3]);
        printf("pbuffer default framebuffer pixel=%u,%u,%u,%u\n", p[0], p[1], p[2], p[3]);
    }

    // The main thread keeps its context current while the worker renders.
    if (surfaceless) {
        ThreadArgs args = { display, config, context };
        pthread_t thread;
        if (pthread_create(&thread, nullptr, threadMain, &args) == 0)
            pthread_join(thread, nullptr);
        else
            CHECK(false, "pthread_create");
        CHECK(eglGetCurrentContext() == context, "main thread lost its current context");
    }

    eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    if (pbuffer != EGL_NO_SURFACE)
        eglDestroySurface(display, pbuffer);
    eglDestroyContext(display, uploader);
    eglDestroyContext(display, context);
    eglTerminate(display);
    surfacelessPlatform();
    if (hardware) {
        auto after = interrupts(observer);
        printf("GPU completed fences %llu -> %llu, EOPs %llu -> %llu\n",
            (unsigned long long)before.completed_fences, (unsigned long long)after.completed_fences,
            (unsigned long long)before.eop_events, (unsigned long long)after.eop_events);
        require(after.completed_fences > before.completed_fences && after.eop_events > before.eop_events,
            "GPU work completed for compositor scene");
        close(observer);
    }
    printf(failures ? "RESULT: FAIL (%d)\n" : "RESULT: PASS\n", failures);
    return failures ? 1 : 0;
}
