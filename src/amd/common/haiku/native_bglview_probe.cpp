/* Copyright 2026 air/OS. SPDX-License-Identifier: MIT
 * Exercise the public Haiku BGLView API through private libGLVND/EGL.
 * Build against the ordinary system GLView.h and private libGL; -lGL -lbe.
 */
#include <Application.h>
#include <Bitmap.h>
#include <GLView.h>
#include <Message.h>
#include <Messenger.h>
#include <Screen.h>
#include <Window.h>
#include <OS.h>
#include "amdgpu_haiku.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <errno.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>

static bool hardware;
static const uint32 kResizeBarrier = 'rszb';
class ProbeView : public BGLView {
public:
   using BGLView::BGLView;
   void MessageReceived(BMessage* message) override
   {
      if (message->what == kResizeBarrier)
         message->SendReply(B_REPLY);
      else
         BGLView::MessageReceived(message);
   }
};
static void require(bool okay, const char *what)
{
   if (okay) return;
   fprintf(stderr, "FAIL: %s GL %#x errno %d\n", what, glGetError(), errno); exit(1);
}
static amdgpu_irq_info irq(int fd)
{
   amdgpu_irq_info info = {}; info.version = AMDGPU_HAIKU_ABI_VERSION; info.size = sizeof(info);
   require(ioctl(fd, AMDGPU_IRQ_INFO, &info, sizeof(info)) == 0, "IRQ query");
   require(info.enabled && info.msi && info.status == B_OK && !info.vm_faults
           && !info.privileged_faults && !info.overflows && !info.unknown, "healthy GPU IRQs");
   return info;
}
static void identity()
{
   const char *renderer = reinterpret_cast<const char*>(glGetString(GL_RENDERER));
   require(renderer != nullptr, "BGLView made a GL context current");
   printf("BGLView renderer: %s; version: %s\n", renderer, glGetString(GL_VERSION));
   require(strstr(renderer, hardware ? "polaris10" : "softpipe") != nullptr, "BGLView renderer selection");
}
static void expected(unsigned x, unsigned y, unsigned w, unsigned h,
                     unsigned blue, bool shared, unsigned char* color)
{
   color[0] = shared ? 37 : x >= w / 2 ? 255 : 0;
   color[1] = shared ? 113 : y >= h / 2 ? 255 : 0;
   color[2] = shared ? 211 : blue; color[3] = 255;
}
static void readback(unsigned w, unsigned h, unsigned blue, bool shared)
{
   std::vector<unsigned char> image(w * h * 4, 0xcd);
   glReadBuffer(GL_BACK); glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, image.data());
   require(glGetError() == GL_NO_ERROR, "BGLView GL readback");
   for (unsigned y = 0; y < h; ++y) for (unsigned x = 0; x < w; ++x) {
      unsigned char color[4]; expected(x, y, w, h, blue, shared, color);
      if (memcmp(image.data() + (y * w + x) * 4, color, 4)) {
         fprintf(stderr, "BGLView readback mismatch %u,%u\n", x, y);
         require(false, "every BGLView framebuffer pixel");
      }
   }
}
static unsigned screenPixels(BBitmap* bitmap, unsigned w, unsigned h, unsigned blue, bool shared)
{
   require(bitmap && bitmap->InitCheck() == B_OK, "app_server bitmap");
   require(bitmap->Bounds().IntegerWidth() + 1 == (int)w
           && bitmap->Bounds().IntegerHeight() + 1 == (int)h, "app_server bitmap dimensions");
   require(bitmap->ColorSpace() == B_RGB32 || bitmap->ColorSpace() == B_RGBA32, "app_server bitmap format");
   unsigned bad = 0;
   for (unsigned y = 0; y < h; ++y) {
      const auto *row = static_cast<const unsigned char*>(bitmap->Bits()) + y * bitmap->BytesPerRow();
      for (unsigned x = 0; x < w; ++x) {
         unsigned char color[4]; expected(x, h - 1 - y, w, h, blue, shared, color);
         const auto *p = row + x * 4;
         if (p[2] != color[0] || p[1] != color[1] || p[0] != color[2]) ++bad;
      }
   }
   return bad;
}
static void present(BWindow* window, BGLView* view, unsigned w, unsigned h, unsigned blue, bool shared)
{
   require(window->Lock(), "window lock for presentation");
   view->Draw(view->Bounds()); view->Sync();
   BRect rectangle = view->ConvertToScreen(view->Bounds()); window->Unlock();
   BScreen screen(window); BBitmap* image = nullptr;
   unsigned bad = 0; bigtime_t deadline = system_time() + 2000000;
   do {
      delete image; image = nullptr;
      require(screen.GetBitmap(&image, false, &rectangle) == B_OK, "app_server capture");
      bad = screenPixels(image, w, h, blue, shared);
      if (!bad) break;
      snooze(20000);
   } while (system_time() < deadline);
   delete image;
   if (bad) fprintf(stderr, "BGLView %ux%u app_server mismatches %u\n", w, h, bad);
   require(!bad, "all BGLView app_server pixels");
}
static void projection(unsigned w, unsigned h)
{
   glViewport(0, 0, w, h);
   glMatrixMode(GL_PROJECTION); glLoadIdentity(); glOrtho(0, w, 0, h, -1, 1);
   glMatrixMode(GL_MODELVIEW); glLoadIdentity();
   glDisable(GL_DITHER); glDisable(GL_BLEND); glDisable(GL_DEPTH_TEST);
   glDisable(GL_LIGHTING); glDisable(GL_SCISSOR_TEST);
}
static void quad(float x0, float y0, float x1, float y1)
{
   glBegin(GL_QUADS);
   glTexCoord2f(0, 0); glVertex2f(x0, y0);
   glTexCoord2f(1, 0); glVertex2f(x1, y0);
   glTexCoord2f(1, 1); glVertex2f(x1, y1);
   glTexCoord2f(0, 1); glVertex2f(x0, y1);
   glEnd();
}
static void sharedFrame(BWindow* window, BGLView* view, GLuint texture)
{
   view->LockGL(); identity(); require(glIsTexture(texture), "BGL_SHARE_CONTEXT texture exists");
   projection(128, 96); glEnable(GL_TEXTURE_2D); glBindTexture(GL_TEXTURE_2D, texture);
   glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
   glColor4ub(255,255,255,255); quad(0, 0, 128, 96);
   readback(128, 96, 0, true); view->SwapBuffers(false);
   require(glGetError() == GL_NO_ERROR, "shared BGLView swap"); view->UnlockGL();
   present(window, view, 128, 96, 0, true);
}
static int run()
{
   int observer = hardware ? open("/dev/" AMDGPU_DEVICE_NAME, O_RDWR) : -1;
   require(!hardware || observer >= 0, "open independent hardware observer");
   auto before = hardware ? irq(observer) : amdgpu_irq_info{};
   auto* window = new BWindow(BRect(40,120,359,311), "WX5100 BGLView probe",
      B_TITLED_WINDOW, B_NOT_CLOSABLE | B_NOT_ZOOMABLE | B_NOT_MINIMIZABLE);
   auto* view = new ProbeView(BRect(0,0,319,191), "legacy GL", B_FOLLOW_ALL,
      B_WILL_DRAW, BGL_RGB | BGL_DOUBLE | BGL_DEPTH);
   window->AddChild(view); window->Show();
   view->LockGL(); identity(); view->UnlockGL();
   const unsigned sizes[][2] = {{320,192},{513,257},{128,96}};
   for (auto& size : sizes) {
      unsigned w = size[0], h = size[1];
      require(window->Lock(), "window lock for resize");
      window->ResizeTo(w-1,h-1); view->ResizeTo(w-1,h-1); view->Sync(); window->Unlock();
      // ResizeTo queues B_VIEW_RESIZED. Sync only flushes app_server requests;
      // wait for the window thread to deliver FrameResized before rendering.
      BMessage request(kResizeBarrier), reply;
      require(BMessenger(view).SendMessage(&request, &reply, 2000000, 2000000) == B_OK
              && reply.what == B_REPLY, "BGLView resize message barrier");
      for (unsigned frame = 0; frame < 4; ++frame) {
         unsigned blue = frame * 85;
         view->LockGL(); projection(w, h); glDisable(GL_TEXTURE_2D);
         for (unsigned y = 0; y < 2; ++y) for (unsigned x = 0; x < 2; ++x) {
            glColor4ub(x ? 255 : 0, y ? 255 : 0, blue, 255);
            quad(x ? w/2 : 0, y ? h/2 : 0, x ? w : w/2, y ? h : h/2);
         }
         readback(w, h, blue, false); view->SwapBuffers(false);
         require(glGetError() == GL_NO_ERROR, "fixed-function BGLView swap"); view->UnlockGL();
         present(window, view, w, h, blue, false);
      }
      printf("PASS: BGLView fixed-function GL, four swaps and exact framebuffer/app_server pixels at %ux%u\n", w, h);
   }
   view->LockGL(); GLuint texture; glGenTextures(1, &texture); glBindTexture(GL_TEXTURE_2D, texture);
   const unsigned char texel[] = {37,113,211,255};
   glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, texel);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST); glFinish();
   // BGL_SHARE_CONTEXT takes the context current on the creating thread.
   auto* shared = new BGLView(BRect(0,0,127,95), "shared GL", B_FOLLOW_ALL,
      B_WILL_DRAW, BGL_RGB | BGL_DOUBLE | BGL_SHARE_CONTEXT);
   view->UnlockGL();
   auto* second = new BWindow(BRect(640,120,767,215), "Shared BGLView",
      B_TITLED_WINDOW, B_NOT_CLOSABLE | B_NOT_ZOOMABLE | B_NOT_MINIMIZABLE);
   second->AddChild(shared); second->Show(); sharedFrame(second, shared, texture);
   require(window->Lock(), "lock parent window for close"); window->Quit();
   sharedFrame(second, shared, texture);
   puts("PASS: BGL_SHARE_CONTEXT texture pixels survive destruction of the creating view");
   shared->LockGL(); glDeleteTextures(1, &texture); shared->UnlockGL();
   require(second->Lock(), "lock shared window for close"); second->Quit();
   if (hardware) {
      auto after = irq(observer);
      require(after.completed_fences > before.completed_fences, "BGLView executed GPU jobs");
      printf("GPU completed %llu -> %llu, EOP %llu -> %llu\n",
         (unsigned long long)before.completed_fences, (unsigned long long)after.completed_fences,
         (unsigned long long)before.eop_events, (unsigned long long)after.eop_events);
      close(observer);
   }
   puts("PASS: ordinary BGLView API, fixed-function GL, resize/presentation, shared context lifetime and teardown");
   return 0;
}
static int32 renderThread(void*)
{
   int result = run(); be_app->PostMessage(B_QUIT_REQUESTED); return result;
}
class ProbeApplication : public BApplication {
public:
   ProbeApplication() : BApplication("application/x-vnd.airOS-native-bglview-probe") {}
   void ReadyToRun() override
   {
      thread = spawn_thread(renderThread, "BGLView rendering", B_NORMAL_PRIORITY, nullptr);
      require(thread >= 0 && resume_thread(thread) == B_OK, "start BGLView rendering");
   }
   thread_id thread = -1;
};
int main(int argc, char** argv)
{
   setvbuf(stdout, nullptr, _IOLBF, 0); disable_debugger(1);
   hardware = argc == 2 && !strcmp(argv[1], "--hardware");
   bool software = argc == 2 && !strcmp(argv[1], "--software");
   require(hardware || software, "usage: --hardware | --software");
   const char* device = getenv("HAIKU_AMDGPU_DEVICE");
   require(software ? !device : device && !strcmp(device, "/dev/" AMDGPU_DEVICE_NAME), "explicit BGL renderer");
   ProbeApplication application; application.Run(); status_t status;
   require(wait_for_thread(application.thread, &status) == B_OK && status == B_OK, "join BGLView thread");
}
