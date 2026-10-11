/* Copyright 2026 air/OS. SPDX-License-Identifier: MIT
 * Private EGL application reset qualification. --manual causes one owned GPU
 * permission fault and explicitly requests one live reset after terminal checks.
 * No observation timeout retries work or resets the device.
 */
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include "amdgpu_haiku.h"
#include <OS.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

static void require(bool okay, const char* why)
{
   if (!okay) { fprintf(stderr, "FAIL: %s EGL %#x GL %#x errno %#x\n", why, eglGetError(), glGetError(), errno); exit(1); }
}
template<class T> static T request() { T r = {}; r.version = AMDGPU_HAIKU_ABI_VERSION; r.size = sizeof(r); return r; }
template<class T> static T query(int fd, uint32 op)
{
   T r = request<T>(); require(!ioctl(fd, op, &r, sizeof(r)), "native query/control"); return r;
}
static int open_device() { int fd = open("/dev/" AMDGPU_DEVICE_NAME, O_RDWR); require(fd >= 0, "native observer"); return fd; }
static amdgpu_irq_info healthy(int fd)
{
   auto r = query<amdgpu_irq_info>(fd, AMDGPU_IRQ_INFO);
   require(r.enabled && r.msi && r.status == B_OK && r.waits == r.completed_fences
      && !r.vm_faults && !r.privileged_faults && !r.unknown && !r.overflows && r.rptr == r.wptr, "healthy hardware retirement");
   return r;
}
struct GL {
   EGLDisplay display; EGLContext context; EGLSurface surface;
   GLuint texture, framebuffer, program, buffer, vs, fs;
   GLenum (*reset)();
};
static GLuint shader(GLenum type, const char* source)
{
   GLuint s = glCreateShader(type); glShaderSource(s, 1, &source, nullptr); glCompileShader(s);
   GLint okay; glGetShaderiv(s, GL_COMPILE_STATUS, &okay); require(okay, "compile shader"); return s;
}
static GL create_gl(bool hardware)
{
   GL g = {}; g.display = eglGetDisplay(EGL_DEFAULT_DISPLAY); EGLint major, minor;
   require(eglInitialize(g.display, &major, &minor), "initialize EGL display");
   require(eglBindAPI(EGL_OPENGL_ES_API), "bind GLES");
   const EGLint ca[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
      EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE};
   EGLConfig config; EGLint count;
   require(eglChooseConfig(g.display, ca, &config, 1, &count) && count == 1, "choose RGBA pbuffer");
   if (hardware) require(strstr(eglQueryString(g.display, EGL_EXTENSIONS), "EGL_EXT_create_context_robustness"), "EGL GLES robustness advertised");
   const EGLint robust[] = {EGL_CONTEXT_CLIENT_VERSION, 2,
      EGL_CONTEXT_OPENGL_RESET_NOTIFICATION_STRATEGY_EXT, EGL_LOSE_CONTEXT_ON_RESET_EXT,
      EGL_CONTEXT_OPENGL_ROBUST_ACCESS_EXT, EGL_TRUE, EGL_NONE};
   const EGLint plain[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
   g.context = eglCreateContext(g.display, config, EGL_NO_CONTEXT, hardware ? robust : plain);
   require(g.context != EGL_NO_CONTEXT, "create GLES robust context with EXT attributes");
   const EGLint sa[] = {EGL_WIDTH, 64, EGL_HEIGHT, 64, EGL_NONE};
   g.surface = eglCreatePbufferSurface(g.display, config, sa);
   require(g.surface != EGL_NO_SURFACE, "create pbuffer");
   require(eglMakeCurrent(g.display, g.surface, g.surface, g.context), "make current robust context");
   const char* renderer = (const char*)glGetString(GL_RENDERER);
   printf("EGL %d.%d robust=%u renderer %s\n", major, minor, hardware, renderer ? renderer : "NULL");
   require(renderer && (hardware ? strstr(renderer, "POLARIS10") || strstr(renderer, "polaris10")
      || strstr(renderer, "WX 5100") : strstr(renderer, "softpipe") != nullptr), "expected hardware/software renderer");
   if (hardware) {
      require(strstr((const char*)glGetString(GL_EXTENSIONS), "GL_KHR_robustness"), "KHR robustness advertised");
      g.reset = (GLenum(*)())eglGetProcAddress("glGetGraphicsResetStatusKHR");
      GLint strategy = 0; glGetIntegerv(GL_RESET_NOTIFICATION_STRATEGY_KHR, &strategy);
      require(g.reset && strategy == GL_LOSE_CONTEXT_ON_RESET_KHR && g.reset() == GL_NO_ERROR
         && glGetError() == GL_NO_ERROR, "loss notification requested and initially healthy");
   }
   glGenTextures(1, &g.texture); glBindTexture(GL_TEXTURE_2D, g.texture);
   glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 64, 64, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
   glGenFramebuffers(1, &g.framebuffer); glBindFramebuffer(GL_FRAMEBUFFER, g.framebuffer);
   glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g.texture, 0);
   require(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE, "complete owned FBO");
   g.vs = shader(GL_VERTEX_SHADER, "attribute vec2 p;void main(){gl_Position=vec4(p,0.,1.);}");
   g.fs = shader(GL_FRAGMENT_SHADER, "precision highp float;void main(){gl_FragColor=vec4(step(32.,gl_FragCoord.x),step(32.,gl_FragCoord.y),0.25,1.);}");
   g.program = glCreateProgram(); glAttachShader(g.program, g.vs); glAttachShader(g.program, g.fs);
   glBindAttribLocation(g.program, 0, "p"); glLinkProgram(g.program); GLint linked;
   glGetProgramiv(g.program, GL_LINK_STATUS, &linked); require(linked, "link shaders"); glUseProgram(g.program);
   const float positions[] = {-1, -1, 3, -1, -1, 3};
   glGenBuffers(1, &g.buffer); glBindBuffer(GL_ARRAY_BUFFER, g.buffer);
   glBufferData(GL_ARRAY_BUFFER, sizeof(positions), positions, GL_STATIC_DRAW);
   glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr); glEnableVertexAttribArray(0);
   return g;
}
static void render()
{
   glViewport(0, 0, 64, 64); glDrawArrays(GL_TRIANGLES, 0, 3); glFinish();
   unsigned char pixels[64*64*4]; memset(pixels, 0xcd, sizeof(pixels));
   glReadPixels(0, 0, 64, 64, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
   require(glGetError() == GL_NO_ERROR, "render/readback succeeds");
   for (unsigned y = 0; y < 64; y++) for (unsigned x = 0; x < 64; x++) {
      auto p = pixels + (y * 64 + x) * 4;
      require(p[0] == (x >= 32 ? 255 : 0) && p[1] == (y >= 32 ? 255 : 0)
         && abs((int)p[2] - 64) <= 1 && p[3] == 255, "all4096 shader pixels");
   }
}
static void destroy_gl(GL& g, bool lost)
{
   if (!lost) {
      glDeleteBuffers(1, &g.buffer); glDeleteProgram(g.program); glDeleteShader(g.vs); glDeleteShader(g.fs);
      glDeleteFramebuffers(1, &g.framebuffer); glDeleteTextures(1, &g.texture);
   }
   require(eglMakeCurrent(g.display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT)
      && eglDestroySurface(g.display, g.surface) && eglDestroyContext(g.display, g.context)
      && eglTerminate(g.display) && eglReleaseThread(), "destroy context, surface and display");
}
static void lost_dispatch()
{
   unsigned char pixels[64]; memset(pixels, 0xcd, sizeof(pixels));
   glClear(GL_COLOR_BUFFER_BIT); glDrawArrays(GL_TRIANGLES, 0, 3); glFinish();
   glReadPixels(0, 0, 4, 4, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
   require(glGetError() == GL_CONTEXT_LOST_KHR, "lost context returns GL_CONTEXT_LOST");
   for (auto p : pixels) require(p == 0xcd, "lost readback never publishes pixels");
}
int main(int argc, char** argv)
{
   setvbuf(stdout, nullptr, _IOLBF, 0); disable_debugger(1);
   bool software = argc == 2 && !strcmp(argv[1], "--software");
   bool absent = argc == 2 && !strcmp(argv[1], "--expect-no-device");
   bool manual = argc == 2 && !strcmp(argv[1], "--manual");
   bool baseline = argc == 2 && !strcmp(argv[1], "--baseline");
   require(software || absent || manual || baseline, "usage --software|--expect-no-device|--baseline|--manual");
   if (absent) {
      EGLDisplay d = eglGetDisplay(EGL_DEFAULT_DISPLAY); EGLint a, b;
      require(!eglInitialize(d, &a, &b), "explicit missing device fails initialization");
      puts("PASS: robust application fixture missing-device handling"); return 0;
   }
   int monitor = software ? -1 : open_device();
   if (!software) query<amdgpu_memory_info>(monitor, AMDGPU_MEMORY_INFO);
   auto memory = software ? amdgpu_memory_info{} : query<amdgpu_memory_info>(monitor, AMDGPU_MEMORY_INFO);
   auto ram = software ? amdgpu_gart_info{} : query<amdgpu_gart_info>(monitor, AMDGPU_GART_INFO);
   auto g = create_gl(!software); render();
   if (!manual) {
      if (!software) require(g.reset() == GL_NO_ERROR, "robust render has no reset");
      destroy_gl(g, false); g = create_gl(!software); render(); destroy_gl(g, false);
   } else {
      auto before = healthy(monitor); auto prior = query<amdgpu_recovery_info>(monitor, AMDGPU_RECOVERY_INFO);
      int bad = open_device(); query<amdgpu_vm_info>(bad, AMDGPU_VM_INFO);
      auto bo = request<amdgpu_buffer>(); bo.bytes = 8192;
      require(!ioctl(bad, AMDGPU_CREATE_SYSTEM_BUFFER, &bo, sizeof(bo))
         && !ioctl(bad, AMDGPU_MAP_BUFFER, &bo, sizeof(bo)), "owned fault buffer");
      auto cpu = (volatile uint32*)(addr_t)bo.address;
      for (unsigned i = 0; i < 2048; i++) { require(cpu[i] == 0, "fault buffer initially zero"); cpu[i] = 0x6b6b6b6b; }
      const uint64 va = 0x300000000ULL; auto m = request<amdgpu_vm_mapping>();
      m.handle = bo.handle; m.address = va; m.bytes = bo.bytes; m.permissions = AMDGPU_VM_READ;
      require(!ioctl(bad, AMDGPU_VM_MAP, &m, sizeof(m)), "owned read-only mapping");
      uint32 words[256]; for (auto& w : words) w = 0x80000000;
      words[0] = 0xc0033700; words[1] = 0x100500; words[2] = uint32(va); words[3] = uint32(va >> 32); words[4] = 0xdead5100;
      auto work = request<amdgpu_gfx_submit>(); work.address = (addr_t)words; work.dwords = 256; work.flags = AMDGPU_GFX_VALIDATE_COMMANDS;
      require(!ioctl(bad, AMDGPU_GFX_SUBMIT_COPY, &work, sizeof(work)) && work.status == B_BAD_DATA,
         "one terminal synchronous owned write-permission fault");
      auto irq = query<amdgpu_irq_info>(monitor, AMDGPU_IRQ_INFO);
      require(irq.status == B_BAD_DATA && irq.vm_faults > before.vm_faults && irq.waits == before.waits + 1,
         "independent IRQ confirms single fault job");
      GLenum first = g.reset(); printf("GL first loss %#x native fault %#x\n", first, unsigned(work.status));
      require(first == GL_UNKNOWN_CONTEXT_RESET_KHR, "application observes unknown reset on its innocent robust context");
      for (unsigned i = 0; i < 3; i++) require(g.reset() == GL_UNKNOWN_CONTEXT_RESET_KHR, "reset remains pending before explicit recovery");
      lost_dispatch();
      require(query<amdgpu_irq_info>(monitor, AMDGPU_IRQ_INFO).waits == irq.waits, "lost GL calls submit no work");
      auto reset = query<amdgpu_recovery_info>(monitor, AMDGPU_GFX_RECOVER_LIVE);
      printf("Explicit live reset status %#x stage %u generation %llu -> %llu\n", unsigned(reset.status), reset.stage,
         (unsigned long long)prior.generation, (unsigned long long)reset.generation);
      require(reset.status == B_OK && reset.stage == 7 && reset.generation == prior.generation + 1,
         "one explicit live reset succeeds");
      bool complete = false;
      for (unsigned i = 0; i < 4 && !complete; i++) {
         GLenum state = g.reset(); printf("GL post-reset status %#x\n", state);
         require(state == GL_UNKNOWN_CONTEXT_RESET_KHR || state == GL_NO_ERROR, "defined reset completion status");
         complete = state == GL_NO_ERROR;
      }
      require(complete && g.reset() == GL_NO_ERROR, "application observes reset completion");
      lost_dispatch(); require(healthy(monitor).completed_fences == 1, "old GL dispatch stays inert after recovery");
      for (unsigned i = 0; i < 2048; i++) require(cpu[i] == 0x6b6b6b6b, "all owned fault-buffer guards survive");
      close(bad); require(delete_area(bo.area) == B_OK, "cleanup fault buffer clone");
      destroy_gl(g, true);
      auto closed = query<amdgpu_recovery_info>(monitor, AMDGPU_RECOVERY_INFO);
      require(!closed.live_handles && !closed.live_vms && !closed.buffers && !closed.vms, "lost EGL teardown closes native resources");
      g = create_gl(true); render(); require(g.reset() == GL_NO_ERROR, "recreated application context is healthy"); destroy_gl(g, false);
      require(healthy(monitor).completed_fences > 1, "new EGL display renders through fresh native device");
   }
   if (!software) {
      auto state = query<amdgpu_recovery_info>(monitor, AMDGPU_RECOVERY_INFO);
      require(!state.live_handles && !state.live_vms && !state.pending && !state.buffers && !state.vms
         && !state.quarantined_ram && !state.quarantined_vram
         && query<amdgpu_memory_info>(monitor, AMDGPU_MEMORY_INFO).allocated_bytes == memory.allocated_bytes
         && query<amdgpu_gart_info>(monitor, AMDGPU_GART_INFO).allocated_bytes == ram.allocated_bytes, "complete EGL resource reclamation");
      auto irq = healthy(monitor); printf("Final healthy completed/waits %llu/%llu\n", (unsigned long long)irq.completed_fences, (unsigned long long)irq.waits);
      close(monitor);
   }
   puts(manual ? "PASS: robust EGL application detects loss, waits for reset completion, rejects old-context rendering, tears down and recreates exact fresh rendering"
      : "PASS: repeated EGL context/display creation, exact shader pixels and complete cleanup");
}
