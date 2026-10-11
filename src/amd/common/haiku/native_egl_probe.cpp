/* Copyright 2026 air/OS. SPDX-License-Identifier: MIT
 * Standalone private-renderer gate. Build with private libglvnd headers/libs
 * and this directory on the include path; link EGL, GLESv2 and be.
 */
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include "amdgpu_haiku.h"
#include <OS.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

static void require(bool ok, const char *what)
{
   if (ok) return;
   fprintf(stderr, "FAIL: %s EGL %#x GL %#x errno %d\n", what, eglGetError(), glGetError(), errno);
   exit(1);
}
static GLuint shader(GLenum type, const char *text)
{
   GLuint id = glCreateShader(type);
   glShaderSource(id, 1, &text, nullptr); glCompileShader(id);
   GLint ok; glGetShaderiv(id, GL_COMPILE_STATUS, &ok);
   if (!ok) { char log[4096]; glGetShaderInfoLog(id, sizeof(log), nullptr, log); puts(log); }
   require(ok, "GLSL compile"); return id;
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
int main(int argc, char **argv)
{
   setvbuf(stdout, nullptr, _IOLBF, 0);
   // Keep assertion/crash diagnostics on stderr and let the test terminate
   // instead of waiting indefinitely for the interactive debugger dialog.
   disable_debugger(1);
   bool autoHardware = argc == 2 && !strcmp(argv[1], "--auto-hardware");
   bool unprivileged = argc == 2 && !strcmp(argv[1], "--unprivileged-auto-software");
   bool autoSoftware = unprivileged || (argc == 2 && !strcmp(argv[1], "--auto-software"));
   bool forcedSoftware = argc == 2 && !strcmp(argv[1], "--forced-software");
   bool hardware = autoHardware || (argc == 2 && !strcmp(argv[1], "--hardware"));
   bool absent = argc == 2 && !strcmp(argv[1], "--expect-no-device");
   bool software = autoSoftware || forcedSoftware || (argc == 2 && !strcmp(argv[1], "--software"));
   require(hardware || absent || software, "usage: --hardware | --software | --expect-no-device | --auto-hardware | --auto-software | --forced-software | --unprivileged-auto-software");
   const char *device = getenv("HAIKU_AMDGPU_DEVICE");
   require(software || autoHardware ? !device : device && !strcmp(device, "/dev/" AMDGPU_DEVICE_NAME), "renderer selection environment");
   if (autoHardware || autoSoftware || forcedSoftware)
      require(!getenv("GALLIUM_DRIVER"), "default Gallium driver selection");
   if (forcedSoftware)
      require(getenv("LIBGL_ALWAYS_SOFTWARE") && !strcmp(getenv("LIBGL_ALWAYS_SOFTWARE"), "1"), "forced software selection");
   if (unprivileged) {
      require(setgid(65534) == 0 && setuid(65534) == 0 && geteuid() == 65534,
              "drop credentials before automatic renderer initialization");
      printf("Automatic renderer selection at UID %u\n", unsigned(geteuid()));
   }
   EGLDisplay display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
   EGLint major = 0, minor = 0;
   EGLBoolean initialized = eglInitialize(display, &major, &minor);
   if (absent) {
      require(!initialized, "explicit missing GPU must not silently use software");
      puts("PASS: explicit missing native GPU fails EGL initialization"); return 0;
   }
   require(initialized, "EGL initialize");
   require(eglBindAPI(EGL_OPENGL_ES_API), "bind GLES");
   const EGLint configAttributes[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
      EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8,
      EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE};
   EGLConfig config; EGLint count;
   require(eglChooseConfig(display, configAttributes, &config, 1, &count) && count == 1, "EGL config");
   const EGLint contextAttributes[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
   EGLContext context = eglCreateContext(display, config, EGL_NO_CONTEXT, contextAttributes);
   const EGLint surfaceAttributes[] = {EGL_WIDTH, 64, EGL_HEIGHT, 64, EGL_NONE};
   EGLSurface surface = eglCreatePbufferSurface(display, config, surfaceAttributes);
   require(context != EGL_NO_CONTEXT && surface != EGL_NO_SURFACE, "EGL context/pbuffer");
   require(eglMakeCurrent(display, surface, surface, context), "make current");
   const char *renderer = (const char*)glGetString(GL_RENDERER);
   printf("EGL %d.%d renderer: %s\nGL version: %s\n", major, minor, renderer,
          (const char*)glGetString(GL_VERSION));
   require(renderer && (hardware ? (strstr(renderer, "POLARIS10") || strstr(renderer, "polaris10") || strstr(renderer, "WX 5100"))
      && !strstr(renderer, "softpipe") && !strstr(renderer, "llvmpipe") : strstr(renderer, "softpipe") != nullptr),
      "expected hardware/software renderer");
   int fd = hardware ? open("/dev/" AMDGPU_DEVICE_NAME, O_RDWR) : -1;
   if (hardware) require(fd >= 0, "open independent hardware observer");
   amdgpu_irq_info before = hardware ? interrupts(fd) : amdgpu_irq_info{};
   GLuint texture, framebuffer;
   glGenTextures(1, &texture); glBindTexture(GL_TEXTURE_2D, texture);
   glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 64, 64, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
   glGenFramebuffers(1, &framebuffer); glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
   glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
   require(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE, "complete FBO");
   glViewport(0, 0, 64, 64); glClearColor(1, 0, 0, 1); glClear(GL_COLOR_BUFFER_BIT);
   unsigned char pixels[64 * 64 * 4]; memset(pixels, 0xcd, sizeof(pixels));
   glReadPixels(0, 0, 64, 64, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
   require(glGetError() == GL_NO_ERROR, "clear/readback");
   for (unsigned i = 0; i < 4096; ++i)
      require(pixels[i*4] == 255 && pixels[i*4+1] == 0 && pixels[i*4+2] == 0 && pixels[i*4+3] == 255,
              "all clear pixels");
   puts("PASS: all 4096 clear pixels");
   GLuint vs = shader(GL_VERTEX_SHADER, "attribute vec2 p; void main(){gl_Position=vec4(p,0.,1.);}");
   GLuint fs = shader(GL_FRAGMENT_SHADER, "precision highp float; void main(){gl_FragColor=vec4(step(32.,gl_FragCoord.x),step(32.,gl_FragCoord.y),0.25,1.);}");
   GLuint program = glCreateProgram(); glAttachShader(program, vs); glAttachShader(program, fs);
   glBindAttribLocation(program, 0, "p"); glLinkProgram(program);
   GLint linked; glGetProgramiv(program, GL_LINK_STATUS, &linked); require(linked, "program link");
   glUseProgram(program);
   const float positions[] = {-1, -1, 3, -1, -1, 3}; GLuint buffer;
   glGenBuffers(1, &buffer); glBindBuffer(GL_ARRAY_BUFFER, buffer);
   glBufferData(GL_ARRAY_BUFFER, sizeof(positions), positions, GL_STATIC_DRAW);
   glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr); glEnableVertexAttribArray(0);
   glDrawArrays(GL_TRIANGLES, 0, 3); glFinish();
   memset(pixels, 0xcd, sizeof(pixels)); glReadPixels(0, 0, 64, 64, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
   require(glGetError() == GL_NO_ERROR, "shader draw/readback");
   unsigned bad = 0;
   for (unsigned y = 0; y < 64; ++y) for (unsigned x = 0; x < 64; ++x) {
      auto p = pixels + (y * 64 + x) * 4;
      if (p[0] != (x >= 32 ? 255 : 0) || p[1] != (y >= 32 ? 255 : 0)
          || abs((int)p[2] - 64) > 1 || p[3] != 255) {
         if (bad++ < 8) printf("pixel %u,%u = %u,%u,%u,%u\n", x, y, p[0], p[1], p[2], p[3]);
      }
   }
   require(!bad, "all 4096 shader pixels");
   if (hardware) {
      auto after = interrupts(fd);
      printf("GPU completed fences %llu -> %llu, EOPs %llu -> %llu\n",
             (unsigned long long)before.completed_fences, (unsigned long long)after.completed_fences,
             (unsigned long long)before.eop_events, (unsigned long long)after.eop_events);
      require(after.completed_fences > before.completed_fences && after.eop_events > before.eop_events,
              "actual GPU completions for GL work");
      close(fd);
   }
   glDeleteBuffers(1, &buffer); glDeleteProgram(program); glDeleteShader(vs); glDeleteShader(fs);
   glDeleteFramebuffers(1, &framebuffer); glDeleteTextures(1, &texture);
   eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
   eglDestroySurface(display, surface); eglDestroyContext(display, context); eglTerminate(display);
   puts("PASS: private EGL, compiled vertex/fragment shaders and all 8192 expected pixels");
}
