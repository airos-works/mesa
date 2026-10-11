/* Copyright 2026 air/OS. SPDX-License-Identifier: MIT
 * Native RadeonSI functional coverage beyond compositor workloads.
 * Build with the private EGL/GLES3 headers, -mtls-dialect=gnu and
 * -lEGL -lGLESv2 -lbe. Does not change any system renderer defaults.
 */
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl31.h>
#include "amdgpu_haiku.h"
#include <OS.h>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <errno.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>

static void require(bool okay, const char *what)
{
   if (okay) return;
   fprintf(stderr, "FAIL: %s EGL %#x GL %#x errno %d\n", what, eglGetError(), glGetError(), errno);
   exit(1);
}
static GLuint shader(GLenum type, const char *source)
{
   GLuint object = glCreateShader(type);
   glShaderSource(object, 1, &source, nullptr); glCompileShader(object);
   GLint okay; glGetShaderiv(object, GL_COMPILE_STATUS, &okay);
   if (!okay) { char log[4096]; glGetShaderInfoLog(object, sizeof(log), nullptr, log); puts(log); }
   require(okay, "GLSL ES 3.10 compile"); return object;
}
static void link(GLuint program)
{
   glLinkProgram(program);
   GLint okay; glGetProgramiv(program, GL_LINK_STATUS, &okay);
   if (!okay) { char log[4096]; glGetProgramInfoLog(program, sizeof(log), nullptr, log); puts(log); }
   require(okay, "GLSL ES 3.10 link");
}
static amdgpu_irq_info irq(int fd)
{
   amdgpu_irq_info info = {};
   info.version = AMDGPU_HAIKU_ABI_VERSION; info.size = sizeof(info);
   require(ioctl(fd, AMDGPU_IRQ_INFO, &info, sizeof(info)) == 0, "independent IRQ query");
   require(info.enabled && info.msi && info.status == B_OK && !info.vm_faults
           && !info.privileged_faults && !info.unknown && !info.overflows,
           "healthy hardware IRQ state");
   return info;
}
static void stage(const char *name, int fd, amdgpu_irq_info& previous)
{
   glFinish(); require(glGetError() == GL_NO_ERROR, name);
   if (fd >= 0) {
      auto current = irq(fd);
      require(current.completed_fences > previous.completed_fences
              && current.eop_events > previous.eop_events, "stage executed GPU work");
      printf("PASS: %s; GPU %llu -> %llu\n", name,
             (unsigned long long)previous.completed_fences, (unsigned long long)current.completed_fences);
      previous = current;
   } else printf("PASS: %s\n", name);
}
static void pixels(bool changed)
{
   std::vector<unsigned char> image(64 * 64 * 4, 0xcd);
   glReadPixels(0, 0, 64, 64, GL_RGBA, GL_UNSIGNED_BYTE, image.data());
   require(glGetError() == GL_NO_ERROR, "pixel readback");
   unsigned bad = 0;
   for (unsigned y = 0; y < 64; ++y) for (unsigned x = 0; x < 64; ++x) {
      const unsigned char *p = image.data() + (y * 64 + x) * 4;
      unsigned char expected[4] = {0, 0, 0, 255};
      if (!changed) expected[x < 32 ? 0 : 1] = 255;
      else if (x < 32) expected[2] = 255;
      else expected[0] = expected[1] = 255;
      if (memcmp(p, expected, 4)) {
         if (bad++ < 4) printf("pixel %u,%u = %u,%u,%u,%u\n", x, y, p[0], p[1], p[2], p[3]);
      }
   }
   require(!bad, "all 4096 indexed/instanced pixels");
}
static GLuint result(GLuint query)
{
   GLuint value = 0xffffffff;
   glGetQueryObjectuiv(query, GL_QUERY_RESULT, &value);
   require(glGetError() == GL_NO_ERROR, "query result"); return value;
}
static void compute_words(GLuint buffer, unsigned guard, uint32_t seed)
{
   const unsigned count = 1024 + 2 * guard;
   glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT | GL_SHADER_STORAGE_BARRIER_BIT);
   glBindBuffer(GL_SHADER_STORAGE_BUFFER, buffer);
   const auto *data = static_cast<const uint32_t*>(glMapBufferRange(
      GL_SHADER_STORAGE_BUFFER, 0, count * sizeof(uint32_t), GL_MAP_READ_BIT));
   require(data != nullptr && glGetError() == GL_NO_ERROR, "map compute results");
   for (unsigned i = 0; i < count; ++i) {
      uint32_t expected = i >= guard && i < guard + 1024
         ? seed ^ ((i - guard) * 1664525u + 1013904223u) : 0xdeadbeef;
      if (data[i] != expected) {
         fprintf(stderr, "compute word %u got %#x expected %#x\n", i, data[i], expected);
         require(false, "all compute results and guard words");
      }
   }
   require(glUnmapBuffer(GL_SHADER_STORAGE_BUFFER), "unmap compute results");
}

int main(int argc, char **argv)
{
   setvbuf(stdout, nullptr, _IOLBF, 0); disable_debugger(1);
   bool hardware = argc == 2 && !strcmp(argv[1], "--hardware");
   bool software = argc == 2 && !strcmp(argv[1], "--software");
   require(hardware || software, "usage: --hardware | --software");
   const char *device = getenv("HAIKU_AMDGPU_DEVICE");
   require(software ? !device : device && !strcmp(device, "/dev/" AMDGPU_DEVICE_NAME), "explicit renderer");
   EGLDisplay display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
   EGLint major, minor;
   require(eglInitialize(display, &major, &minor) && eglBindAPI(EGL_OPENGL_ES_API), "EGL initialize");
   const EGLint configAttributes[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
      EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT_KHR, EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8,
      EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE};
   EGLConfig config; EGLint count;
   require(eglChooseConfig(display, configAttributes, &config, 1, &count) && count == 1, "GLES3 config");
   const EGLint contextAttributes[] = {EGL_CONTEXT_MAJOR_VERSION_KHR, 3,
      EGL_CONTEXT_MINOR_VERSION_KHR, 1, EGL_NONE};
   EGLContext context = eglCreateContext(display, config, EGL_NO_CONTEXT, contextAttributes);
   const EGLint surfaceAttributes[] = {EGL_WIDTH, 64, EGL_HEIGHT, 64, EGL_NONE};
   EGLSurface surface = eglCreatePbufferSurface(display, config, surfaceAttributes);
   require(context != EGL_NO_CONTEXT && surface != EGL_NO_SURFACE
           && eglMakeCurrent(display, surface, surface, context), "GLES3.1 context");
   const char *renderer = reinterpret_cast<const char*>(glGetString(GL_RENDERER));
   printf("EGL %d.%d renderer: %s\nGL version: %s\n", major, minor, renderer,
          reinterpret_cast<const char*>(glGetString(GL_VERSION)));
   GLint glMajor, glMinor; glGetIntegerv(GL_MAJOR_VERSION, &glMajor); glGetIntegerv(GL_MINOR_VERSION, &glMinor);
   require(glMajor > 3 || (glMajor == 3 && glMinor >= 1), "advertised GLES3.1");
   require(renderer && (hardware ? (strstr(renderer, "polaris10") || strstr(renderer, "WX 5100"))
      && !strstr(renderer, "softpipe") && !strstr(renderer, "llvmpipe") : strstr(renderer, "softpipe") != nullptr),
      "hardware/software renderer identity");
   int observer = hardware ? open(device, O_RDWR) : -1;
   require(!hardware || observer >= 0, "open independent GPU observer");
   amdgpu_irq_info previous = hardware ? irq(observer) : amdgpu_irq_info{};

   GLuint color, fbo, depth;
   glGenTextures(1, &color); glBindTexture(GL_TEXTURE_2D, color);
   glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, 64, 64);
   glGenFramebuffers(1, &fbo); glBindFramebuffer(GL_FRAMEBUFFER, fbo);
   glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, color, 0);
   glGenRenderbuffers(1, &depth); glBindRenderbuffer(GL_RENDERBUFFER, depth);
   glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, 64, 64);
   glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, depth);
   require(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE, "depth/stencil FBO");
   glViewport(0, 0, 64, 64); glDisable(GL_DITHER); glDisable(GL_BLEND);
   glClearColor(0, 0, 0, 1); glClearDepthf(1); glClearStencil(0);
   glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);

   GLuint vs = shader(GL_VERTEX_SHADER, "#version 310 es\nlayout(location=0) in vec2 p; uniform float depth; flat out int which; void main(){which=gl_InstanceID; gl_Position=vec4(p.x*.5+(which==0?-.5:.5),p.y,depth,1.);}");
   GLuint fs = shader(GL_FRAGMENT_SHADER, "#version 310 es\nprecision highp float; precision highp int; flat in int which; layout(std140) uniform Colors{vec4 colors[2];}; layout(location=0) out vec4 result; void main(){result=colors[which];}");
   GLuint program = glCreateProgram(); glAttachShader(program, vs); glAttachShader(program, fs); link(program);
   glUseProgram(program);
   const float positions[] = {-1,-1, 1,-1, 1,1, -1,1};
   const GLushort indices[] = {0,1,2, 2,3,0};
   const float colors[] = {1,0,0,1, 0,1,0,1};
   GLuint vao, buffers[4], queries[2]; glGenVertexArrays(1, &vao); glBindVertexArray(vao);
   glGenBuffers(4, buffers); glGenQueries(2, queries);
   glBindBuffer(GL_ARRAY_BUFFER, buffers[0]); glBufferData(GL_ARRAY_BUFFER, sizeof(positions), positions, GL_STATIC_DRAW);
   glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr); glEnableVertexAttribArray(0);
   glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, buffers[1]); glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(indices), indices, GL_STATIC_DRAW);
   glBindBuffer(GL_UNIFORM_BUFFER, buffers[2]); glBufferData(GL_UNIFORM_BUFFER, sizeof(colors), colors, GL_DYNAMIC_DRAW);
   GLuint block = glGetUniformBlockIndex(program, "Colors"); require(block != GL_INVALID_INDEX, "uniform block");
   glUniformBlockBinding(program, block, 2); glBindBufferBase(GL_UNIFORM_BUFFER, 2, buffers[2]);
   GLint depthLocation = glGetUniformLocation(program, "depth"); require(depthLocation >= 0, "depth uniform");
   glUniform1f(depthLocation, 0);
   glEnable(GL_DEPTH_TEST); glDepthFunc(GL_LESS);
   glEnable(GL_STENCIL_TEST); glStencilFunc(GL_ALWAYS, 3, 255); glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
   glBeginQuery(GL_ANY_SAMPLES_PASSED, queries[0]);
   glDrawElementsInstanced(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, nullptr, 2);
   glEndQuery(GL_ANY_SAMPLES_PASSED); require(result(queries[0]) == GL_TRUE, "visible occlusion query");
   pixels(false); stage("indexed instancing, UBO colors, depth/stencil writes and positive query", observer, previous);
   glUniform1f(depthLocation, .5f);
   glBeginQuery(GL_ANY_SAMPLES_PASSED, queries[0]);
   glDrawElementsInstanced(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, nullptr, 2);
   glEndQuery(GL_ANY_SAMPLES_PASSED); require(result(queries[0]) == GL_FALSE, "occluded query");
   pixels(false); stage("depth rejects covered geometry and query returns zero", observer, previous);
   glDisable(GL_DEPTH_TEST); glStencilFunc(GL_EQUAL, 4, 255);
   glBeginQuery(GL_ANY_SAMPLES_PASSED, queries[0]);
   glDrawElementsInstanced(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, nullptr, 2);
   glEndQuery(GL_ANY_SAMPLES_PASSED); require(result(queries[0]) == GL_FALSE, "stencil rejection query");
   pixels(false); stage("stencil rejects mismatched reference and preserves all pixels", observer, previous);
   glStencilFunc(GL_EQUAL, 3, 255);
   const float changedColors[] = {0,0,1,1, 1,1,0,1};
   glBindBuffer(GL_UNIFORM_BUFFER, buffers[2]); glBufferSubData(GL_UNIFORM_BUFFER, 0, sizeof(changedColors), changedColors);
   // ES3.1 DrawElementsIndirectCommand: count, instances, firstIndex, baseVertex, reserved zero.
   const uint32_t draw[] = {6,2,0,0,0};
   glBindBuffer(GL_DRAW_INDIRECT_BUFFER, buffers[3]); glBufferData(GL_DRAW_INDIRECT_BUFFER, sizeof(draw), draw, GL_STATIC_DRAW);
   glDrawElementsIndirect(GL_TRIANGLES, GL_UNSIGNED_SHORT, nullptr);
   GLsync fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0); require(fence != nullptr, "GL fence");
   GLenum waited = glClientWaitSync(fence, GL_SYNC_FLUSH_COMMANDS_BIT, 5000000000ULL);
   require(waited == GL_ALREADY_SIGNALED || waited == GL_CONDITION_SATISFIED, "GL fence completion");
   glDeleteSync(fence); pixels(true);
   stage("indexed indirect draw, changed UBO, stencil acceptance and GL fence", observer, previous);
   glDisable(GL_STENCIL_TEST);

   GLuint tfvs = shader(GL_VERTEX_SHADER, "#version 310 es\nprecision highp int; out uvec2 captured; void main(){uint i=uint(gl_VertexID); captured=uvec2(i,i*17u+3u); gl_Position=vec4(0.,0.,0.,1.);}");
   GLuint tffs = shader(GL_FRAGMENT_SHADER, "#version 310 es\nprecision highp float; out vec4 result; void main(){result=vec4(1.);}");
   GLuint tfprogram = glCreateProgram(); glAttachShader(tfprogram, tfvs); glAttachShader(tfprogram, tffs);
   const char *varying = "captured"; glTransformFeedbackVaryings(tfprogram, 1, &varying, GL_INTERLEAVED_ATTRIBS); link(tfprogram);
   GLuint tfvao, tf, tfbuffer; glGenVertexArrays(1, &tfvao); glBindVertexArray(tfvao);
   glGenTransformFeedbacks(1, &tf); glBindTransformFeedback(GL_TRANSFORM_FEEDBACK, tf);
   glGenBuffers(1, &tfbuffer); glBindBuffer(GL_TRANSFORM_FEEDBACK_BUFFER, tfbuffer);
   uint32_t initial[96]; std::fill(initial, initial + 96, 0xaabbccdd);
   glBufferData(GL_TRANSFORM_FEEDBACK_BUFFER, sizeof(initial), initial, GL_DYNAMIC_READ);
   glBindBufferRange(GL_TRANSFORM_FEEDBACK_BUFFER, 0, tfbuffer, 16 * sizeof(uint32_t), 64 * sizeof(uint32_t));
   glUseProgram(tfprogram); glEnable(GL_RASTERIZER_DISCARD);
   glBeginQuery(GL_TRANSFORM_FEEDBACK_PRIMITIVES_WRITTEN, queries[1]);
   glBeginTransformFeedback(GL_POINTS); glDrawArrays(GL_POINTS, 0, 32); glEndTransformFeedback();
   glEndQuery(GL_TRANSFORM_FEEDBACK_PRIMITIVES_WRITTEN); glDisable(GL_RASTERIZER_DISCARD);
   require(result(queries[1]) == 32, "transform feedback primitive count");
   const auto *captured = static_cast<const uint32_t*>(glMapBufferRange(GL_TRANSFORM_FEEDBACK_BUFFER, 0, sizeof(initial), GL_MAP_READ_BIT));
   require(captured != nullptr, "map transform feedback");
   for (unsigned i = 0; i < 96; ++i) {
      uint32_t expected = i < 16 || i >= 80 ? 0xaabbccdd
         : (i & 1) ? ((i - 16) / 2) * 17 + 3 : (i - 16) / 2;
      if (captured[i] != expected) {
         fprintf(stderr, "feedback word %u got %#x expected %#x\n", i, captured[i], expected);
         require(false, "all transform feedback words and guards");
      }
   }
   require(glUnmapBuffer(GL_TRANSFORM_FEEDBACK_BUFFER), "unmap transform feedback");
   stage("transform feedback data, guarded buffer range and primitive query", observer, previous);

   GLuint cs = shader(GL_COMPUTE_SHADER, "#version 310 es\nprecision highp int; layout(local_size_x=64) in; layout(std430,binding=0) buffer Data{uint words[];}; uniform uint seed; void main(){uint i=gl_GlobalInvocationID.x; words[i]=seed^(i*1664525u+1013904223u);}");
   GLuint compute = glCreateProgram(); glAttachShader(compute, cs); link(compute); glUseProgram(compute);
   GLint alignment; glGetIntegerv(GL_SHADER_STORAGE_BUFFER_OFFSET_ALIGNMENT, &alignment);
   require(alignment > 0 && alignment <= 1048576 && alignment % 4 == 0, "SSBO alignment");
   unsigned guard = ((64 + alignment - 1) / alignment * alignment) / 4;
   std::vector<uint32_t> data(1024 + 2 * guard, 0xdeadbeef);
   GLuint storage, dispatch; glGenBuffers(1, &storage); glBindBuffer(GL_SHADER_STORAGE_BUFFER, storage);
   glBufferData(GL_SHADER_STORAGE_BUFFER, data.size() * sizeof(uint32_t), data.data(), GL_DYNAMIC_READ);
   glBindBufferRange(GL_SHADER_STORAGE_BUFFER, 0, storage, guard * sizeof(uint32_t), 1024 * sizeof(uint32_t));
   GLint seed = glGetUniformLocation(compute, "seed"); require(seed >= 0, "compute seed");
   glUniform1ui(seed, 0x12345678); glDispatchCompute(16, 1, 1); compute_words(storage, guard, 0x12345678);
   stage("direct compute, SSBO range, memory barrier and 1024 exact results/guards", observer, previous);
   const uint32_t groups[] = {16,1,1}; glGenBuffers(1, &dispatch); glBindBuffer(GL_DISPATCH_INDIRECT_BUFFER, dispatch);
   glBufferData(GL_DISPATCH_INDIRECT_BUFFER, sizeof(groups), groups, GL_STATIC_DRAW);
   glUniform1ui(seed, 0x9e3779b9); glDispatchComputeIndirect(0); compute_words(storage, guard, 0x9e3779b9);
   stage("indirect compute and replacement SSBO results/guards", observer, previous);

   glUseProgram(0); glBindTransformFeedback(GL_TRANSFORM_FEEDBACK, 0); glBindVertexArray(0);
   glDeleteBuffers(1, &dispatch); glDeleteBuffers(1, &storage); glDeleteProgram(compute); glDeleteShader(cs);
   glDeleteBuffers(1, &tfbuffer); glDeleteTransformFeedbacks(1, &tf); glDeleteVertexArrays(1, &tfvao);
   glDeleteProgram(tfprogram); glDeleteShader(tfvs); glDeleteShader(tffs);
   glDeleteQueries(2, queries); glDeleteBuffers(4, buffers); glDeleteVertexArrays(1, &vao);
   glDeleteProgram(program); glDeleteShader(vs); glDeleteShader(fs);
   glDeleteRenderbuffers(1, &depth); glDeleteFramebuffers(1, &fbo); glDeleteTextures(1, &color);
   require(glGetError() == GL_NO_ERROR, "resource teardown");
   eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
   eglDestroySurface(display, surface); eglDestroyContext(display, context); eglTerminate(display);
   if (hardware) {
      auto final = irq(observer);
      printf("GPU final completed %llu EOP %llu waits %llu\n", (unsigned long long)final.completed_fences,
             (unsigned long long)final.eop_events, (unsigned long long)final.waits);
      close(observer);
   }
   puts("PASS: GLES3.1 indexed/indirect/instanced drawing, UBO/depth/stencil/queries, transform feedback and direct/indirect SSBO compute");
}
