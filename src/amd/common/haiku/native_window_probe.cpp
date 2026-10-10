/* Copyright 2026 air/OS. SPDX-License-Identifier: MIT
 * Private EGL window publication and app_server pixel gate.
 * Build with Mesa's src/gallium/winsys/sw/hgl include path, private EGL/GLES2,
 * -std=c++17 -mtls-dialect=gnu, and -lEGL -lGLESv2 -lbe.
 */
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <Application.h>
#include <Bitmap.h>
#include <Locker.h>
#include <OS.h>
#include <Screen.h>
#include <View.h>
#include <Window.h>
#include <stdint.h>
#include "hgl_sw_winsys.h"
#include "amdgpu_haiku.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>

static void require(bool ok, const char* message)
{
   if (ok) return;
   fprintf(stderr, "FAIL: %s EGL=%#x GL=%#x\n", message, eglGetError(), glGetError());
   exit(1);
}
static amdgpu_irq_info interrupts(int fd)
{
   amdgpu_irq_info info = {};
   info.version = AMDGPU_HAIKU_ABI_VERSION; info.size = sizeof(info);
   require(ioctl(fd, AMDGPU_IRQ_INFO, &info, sizeof(info)) == 0, "IRQ query");
   require(info.enabled && info.msi && info.status == B_OK && !info.vm_faults
      && !info.privileged_faults && !info.unknown && !info.overflows, "healthy GPU IRQs");
   return info;
}
static GLuint shader(GLenum type, const char* source)
{
   GLuint id = glCreateShader(type);
   glShaderSource(id, 1, &source, nullptr); glCompileShader(id);
   GLint ok = 0; glGetShaderiv(id, GL_COMPILE_STATUS, &ok);
   require(ok, "shader compile"); return id;
}
static unsigned checkPixels(const BBitmap* bitmap, unsigned width, unsigned height,
   int blue, bool checkAlpha, bool report = true)
{
   require(bitmap && bitmap->InitCheck() == B_OK, "published bitmap");
   require(bitmap->Bounds().IntegerWidth() + 1 == (int)width
      && bitmap->Bounds().IntegerHeight() + 1 == (int)height, "bitmap dimensions");
   require(bitmap->ColorSpace() == B_RGB32 || bitmap->ColorSpace() == B_RGBA32, "bitmap color space");
   unsigned bad = 0;
   for (unsigned y = 0; y < height; ++y) {
      const auto* row = static_cast<const unsigned char*>(bitmap->Bits()) + y * bitmap->BytesPerRow();
      for (unsigned x = 0; x < width; ++x) {
         const auto* p = row + x * 4;
         // Haiku bitmap rows are top-to-bottom; GL window coordinates start at the bottom.
         if (p[2] != (x >= width / 2 ? 255 : 0)
            || p[1] != (height - 1 - y >= height / 2 ? 255 : 0)
            || abs((int)p[0] - blue) > 1 || (checkAlpha && p[3] != 255)) {
            if (bad++ < 4 && report) printf("pixel %u,%u BGRA=%u,%u,%u,%u\n", x,y,p[0],p[1],p[2],p[3]);
         }
      }
   }
   return bad;
}

class Hook : public BitmapHook {
public:
   void GetSize(uint32_t& width, uint32_t& height) override
   { lock.Lock(); width = w; height = h; lock.Unlock(); }
   BBitmap* SetBitmap(BBitmap* bitmap) override
   {
      lock.Lock(); BBitmap* old = current; current = bitmap;
      if (bitmap) ++publications;
      lock.Unlock(); return old;
   }
   void Resize(unsigned width, unsigned height)
   { lock.Lock(); w = width; h = height; lock.Unlock(); }
   void Draw(BView* view)
   { lock.Lock(); if (current) view->DrawBitmap(current, BPoint(0,0)); lock.Unlock(); }
   void Check(int blue)
   {
      lock.Lock(); unsigned bad = checkPixels(current,w,h,blue,true); lock.Unlock();
      printf("published bitmap %ux%u checked, %u bad pixels\n",w,h,bad);
      require(!bad, "all published bitmap pixels");
   }
   bool Empty() { lock.Lock(); bool empty = !current; lock.Unlock(); return empty; }
   unsigned publications = 0;
private:
   BLocker lock;
   BBitmap* current = nullptr; // The EGL backend owns and retires this bitmap.
   unsigned w = 320, h = 192;
};
class TestView : public BView {
public:
   explicit TestView(Hook& hook) : BView(BRect(0,0,319,191), "pixels", B_FOLLOW_NONE, B_WILL_DRAW), hook(hook) {}
   void Draw(BRect) override { hook.Draw(this); }
private:
   Hook& hook;
};

static int run()
{
   const char* device = getenv("HAIKU_AMDGPU_DEVICE");
   int observer = device ? open(device,O_RDWR) : -1;
   if (device) require(observer >= 0,"GPU observer");
   auto before = device ? interrupts(observer) : amdgpu_irq_info{};
   Hook hook;
   auto* window = new BWindow(BRect(80,100,399,291),"WX5100 EGL window probe",B_TITLED_WINDOW,
      B_NOT_CLOSABLE | B_NOT_ZOOMABLE | B_NOT_MINIMIZABLE | B_NOT_RESIZABLE);
   auto* view = new TestView(hook); window->AddChild(view); window->Show();
   EGLDisplay display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
   EGLint major,minor; require(eglInitialize(display,&major,&minor),"EGL initialize");
   require(eglBindAPI(EGL_OPENGL_ES_API),"GLES API");
   const EGLint attrs[] = {EGL_SURFACE_TYPE,EGL_WINDOW_BIT,EGL_RENDERABLE_TYPE,EGL_OPENGL_ES2_BIT,
      EGL_RED_SIZE,8,EGL_GREEN_SIZE,8,EGL_BLUE_SIZE,8,EGL_ALPHA_SIZE,8,EGL_NONE};
   EGLConfig config; EGLint count;
   require(eglChooseConfig(display,attrs,&config,1,&count) && count == 1,"window config");
   const EGLint ca[] = {EGL_CONTEXT_CLIENT_VERSION,2,EGL_NONE};
   EGLContext context = eglCreateContext(display,config,EGL_NO_CONTEXT,ca);
   EGLSurface surface = eglCreateWindowSurface(display,config,reinterpret_cast<EGLNativeWindowType>(&hook),nullptr);
   require(context != EGL_NO_CONTEXT && surface != EGL_NO_SURFACE,"context/window surface");
   require(eglMakeCurrent(display,surface,surface,context),"make current");
   const char* renderer = reinterpret_cast<const char*>(glGetString(GL_RENDERER));
   printf("EGL %d.%d renderer=%s\n",major,minor,renderer);
   require(renderer && strstr(renderer,device ? "polaris10" : "softpipe"),"expected renderer");
   GLuint vs = shader(GL_VERTEX_SHADER,"attribute vec2 p;void main(){gl_Position=vec4(p,0.,1.);}");
   GLuint fs = shader(GL_FRAGMENT_SHADER,"precision highp float;uniform vec2 extent;uniform float blue;void main(){gl_FragColor=vec4(step(extent.x*.5,gl_FragCoord.x),step(extent.y*.5,gl_FragCoord.y),blue,1.);}");
   GLuint program = glCreateProgram(); glAttachShader(program,vs); glAttachShader(program,fs);
   glBindAttribLocation(program,0,"p");glLinkProgram(program);
   GLint linked; glGetProgramiv(program,GL_LINK_STATUS,&linked);require(linked,"link");glUseProgram(program);
   GLint extent=glGetUniformLocation(program,"extent"),blue=glGetUniformLocation(program,"blue");
   const float vertices[] = {-1,-1,3,-1,-1,3}; GLuint vbo;
   glGenBuffers(1,&vbo);glBindBuffer(GL_ARRAY_BUFFER,vbo);glBufferData(GL_ARRAY_BUFFER,sizeof(vertices),vertices,GL_STATIC_DRAW);
   glVertexAttribPointer(0,2,GL_FLOAT,GL_FALSE,0,nullptr);glEnableVertexAttribArray(0);
   const unsigned sizes[][2] = {{320,192},{513,257},{128,96}};
   for (const auto& size : sizes) {
      unsigned w=size[0],h=size[1];hook.Resize(w,h);
      require(window->Lock(),"lock window for resize");
      window->ResizeTo(w-1,h-1);view->ResizeTo(w-1,h-1);view->Sync();window->Unlock();
      EGLint ew,eh;require(eglQuerySurface(display,surface,EGL_WIDTH,&ew)
         && eglQuerySurface(display,surface,EGL_HEIGHT,&eh) && ew==(int)w && eh==(int)h,"resized EGL dimensions");
      glViewport(0,0,w,h);glUniform2f(extent,w,h);
      for (int frame=0;frame<12;++frame) {
         float b=(frame%4)*.25f;glUniform1f(blue,b);glDrawArrays(GL_TRIANGLES,0,3);
         require(eglSwapBuffers(display,surface),"window swap");
         require(glGetError()==GL_NO_ERROR,"window GL status");hook.Check((int)(b*255+.5f));
         require(window->Lock(),"lock window for draw");view->Draw(view->Bounds());view->Sync();window->Unlock();
      }
      // Inspect app_server's pixels independently of the EGL-published bitmap.
      require(window->Lock(),"lock window for screen coordinates");
      BRect rect=view->ConvertToScreen(view->Bounds());window->Unlock();
      BScreen screen(window);BBitmap* screenshot=nullptr;
      unsigned bad;bigtime_t deadline=system_time()+2000000;
      // Sync completes view drawing commands; desktop composition can follow
      // asynchronously. Observe this same frame until it is visible or times out.
      do {
         delete screenshot;screenshot=nullptr;
         require(screen.GetBitmap(&screenshot,false,&rect)==B_OK,"screen capture");
         bad=checkPixels(screenshot,w,h,191,false,false);
         if (!bad) break;
         snooze(20000);
      } while (system_time()<deadline);
      if (bad) checkPixels(screenshot,w,h,191,false);
      delete screenshot;
      printf("app_server %ux%u checked, %u bad pixels\n",w,h,bad);require(!bad,"all app_server pixels");
   }
   require(hook.publications>=36,"all swaps published");
   glDeleteBuffers(1,&vbo);glDeleteProgram(program);glDeleteShader(vs);glDeleteShader(fs);
   eglMakeCurrent(display,EGL_NO_SURFACE,EGL_NO_SURFACE,EGL_NO_CONTEXT);
   require(eglDestroySurface(display,surface),"destroy window surface");require(hook.Empty(),"bitmap detached before teardown");
   eglDestroyContext(display,context);eglTerminate(display);
   require(window->Lock(),"lock window for quit");window->Quit();
   if (device) {
      auto after=interrupts(observer);
      printf("GPU completed fences %llu -> %llu, EOPs %llu -> %llu\n",(unsigned long long)before.completed_fences,
         (unsigned long long)after.completed_fences,(unsigned long long)before.eop_events,(unsigned long long)after.eop_events);
      require(after.completed_fences>before.completed_fences,"GPU work during window probe");close(observer);
   }
   puts("PASS: EGL window swaps, resize, bitmap and app_server pixels, detach and teardown");
   return 0;
}

static int32 renderThread(void*)
{
   int result = run();
   be_app->PostMessage(B_QUIT_REQUESTED);
   return result;
}
class ProbeApplication : public BApplication {
public:
   ProbeApplication() : BApplication("application/x-vnd.airOS-native-window-probe") {}
   void ReadyToRun() override
   {
      thread = spawn_thread(renderThread,"EGL window rendering",B_NORMAL_PRIORITY,nullptr);
      require(thread >= 0 && resume_thread(thread) == B_OK,"start render thread");
   }
   thread_id thread = -1;
};
int main()
{
   setvbuf(stdout,nullptr,_IONBF,0);disable_debugger(1);
   ProbeApplication app;app.Run();
   status_t result;
   require(app.thread >= 0 && wait_for_thread(app.thread,&result) == B_OK,"join render thread");
   return result;
}
