/* SPDX-License-Identifier: MIT */
#include <fcntl.h>
#include <unistd.h>
#include "pipe/p_screen.h"
#include "util/driconf.h"
#include "util/xmlconfig.h"
#include "util/macros.h"
#include "pan_public.h"
static const driOptionDescription haiku_panfrost_options[] = {
#include "driinfo_panfrost.h"
};
struct pipe_screen *panfrost_haiku_screen_create(const char *device)
{
   int fd = open(device, O_RDWR | O_CLOEXEC);
   if (fd < 0) return NULL;
   driOptionCache info = {0}, options = {0};
   driParseOptionInfo(&info, haiku_panfrost_options, ARRAY_SIZE(haiku_panfrost_options));
   const struct pipe_screen_config config = {.options = &options, .options_info = &info};
   struct pipe_screen *screen = panfrost_create_screen(fd, &config, NULL);
   driDestroyOptionCache(&options);
   driDestroyOptionInfo(&info);
   return screen;
}
