/* Anland consumer that checks every received frame for a solid palette
 * color (sync-stress client fullscreen). Mixed or foreign pixels in a frame
 * mean somebody read a buffer before its producer finished. Throwaway.
 * usage: consumer-sync socket dma-heap max-frames   (samples while ./sample exists) */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/dma-buf.h>
#include <linux/dma-heap.h>
#include <linux/sync_file.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include "transport/libdisplay_consumer/display_consumer.h"

enum { WIDTH = 2392, HEIGHT = 1080, STRIDE = 9600, SLOTS = 5, RGBA8888 = 1 };
static const size_t bytes = (size_t)STRIDE * HEIGHT;
static bool active;

static uint64_t now_ns(void)
{
   struct timespec ts;
   clock_gettime(CLOCK_MONOTONIC, &ts);
   return (uint64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
}
static void fail(const char *what) { fprintf(stderr, "ERROR %s errno=%d\n", what, errno); exit(2); }
static void ready(void *u) { (void)u; active = true; }
static void lost(void *u) { (void)u; active = false; }

static void sync_cpu(int fd, uint64_t flags)
{
   struct dma_buf_sync req = { .flags = DMA_BUF_SYNC_READ | flags };
   int ret;
   do { ret = ioctl(fd, DMA_BUF_IOCTL_SYNC, &req); } while (ret < 0 && errno == EINTR);
   if (ret < 0) fail("DMA_BUF_IOCTL_SYNC");
}

static void wait_fence(int fd)
{
   struct pollfd p = { .fd = fd, .events = POLLIN };
   int ret;
   do { ret = poll(&p, 1, 3000); } while (ret < 0 && errno == EINTR);
   if (ret != 1 || !(p.revents & POLLIN)) fail("producer fence");
}

/* 0,1,2 = palette index; 3 = other (background, garbage). */
static int classify(const uint8_t *p)
{
   static const uint8_t pal[3][3] = { { 0xd0, 0x30, 0x30 }, { 0x30, 0xd0, 0x30 }, { 0x30, 0x30, 0xd0 } };
   for (int i = 0; i < 3; i++)
      if (abs(p[0] - pal[i][0]) <= 3 && abs(p[1] - pal[i][1]) <= 3 && abs(p[2] - pal[i][2]) <= 3)
         return i;
   return 3;
}

static void save_frame(const char *path, const uint8_t *data)
{
   FILE *f = fopen(path, "wb");
   if (!f) return;
   fprintf(f, "P6\n%d %d\n255\n", WIDTH, HEIGHT);
   static uint8_t rgb[WIDTH * 3];
   for (int y = 0; y < HEIGHT; ++y) {
      const uint8_t *row = data + (size_t)y * STRIDE;
      for (int x = 0; x < WIDTH; ++x) memcpy(rgb + 3 * x, row + 4 * x, 3);
      fwrite(rgb, sizeof rgb, 1, f);
   }
   fclose(f);
}

int main(int argc, char **argv)
{
   if (argc != 4) return 2;
   unsigned long limit = strtoul(argv[3], NULL, 10);
   setvbuf(stdout, NULL, _IOLBF, 0);
   int heap = open(argv[2], O_RDONLY | O_CLOEXEC);
   if (heap < 0) fail("open heap");
   struct buf_info layouts[SLOTS];
   int fds[SLOTS];
   const uint8_t *maps[SLOTS];
   for (int i = 0; i < SLOTS; ++i) {
      struct dma_heap_allocation_data req = { .len = bytes, .fd_flags = O_RDWR | O_CLOEXEC };
      if (ioctl(heap, DMA_HEAP_IOCTL_ALLOC, &req) < 0) fail("alloc");
      fds[i] = (int)req.fd;
      maps[i] = mmap(NULL, bytes, PROT_READ, MAP_SHARED, fds[i], 0);
      if (maps[i] == MAP_FAILED) fail("mmap");
      layouts[i] = (struct buf_info){ .stride = STRIDE, .width = WIDTH, .height = HEIGHT,
                                      .format = RGBA8888, .modifier = 0, .offset = 0 };
   }
   close(heap);
   display_ctx *ctx = NULL;
   if (connect_to_deamon(&ctx, argv[1]) || set_screen_info(ctx, WIDTH, HEIGHT, RGBA8888, 120000) ||
       set_fallback_callback(ctx, lost, NULL) || set_exit_fallback_callback(ctx, ready, NULL) ||
       push_dmabufs(ctx, fds, layouts, SLOTS)) fail("handshake");
   printf("CONSUMER_READY\n");
   const struct timespec retry = { .tv_nsec = 4000000 };
   const uint64_t deadline = now_ns() + 60000000000ull;
   unsigned samples = 0, solid = 0, mixed = 0, foreign = 0, saved = 0;
   unsigned color_hist[4] = {0};
   uint64_t t0 = 0;
   for (unsigned seq = 0; samples < limit; ++seq) {
      const unsigned slot = seq % SLOTS;
      uint64_t begin;
      do {
         begin = now_ns();
         if (select_dmabuf(ctx, (int)slot)) fail("select");
         if (!active) { if (begin > deadline) fail("producer timeout"); nanosleep(&retry, NULL); }
      } while (!active);
      int fd = refresh_done(ctx);
      if (fd >= 0) { wait_fence(fd); close(fd); }
      if (access("stop", F_OK) == 0) break;
      if (access("sample", F_OK) == 0 && fd >= 0) {
         if (!t0) t0 = begin;
         ++samples;
         sync_cpu(fds[slot], DMA_BUF_SYNC_START);
         /* Grid 64x32 over the whole output. */
         unsigned cnt[4] = {0};
         for (int gy = 0; gy < 32; gy++)
            for (int gx = 0; gx < 64; gx++) {
               int x = 8 + gx * (WIDTH - 16) / 63, y = 8 + gy * (HEIGHT - 16) / 31;
               cnt[classify(maps[slot] + (size_t)y * STRIDE + 4 * x)]++;
            }
         int kinds = (cnt[0] > 0) + (cnt[1] > 0) + (cnt[2] > 0);
         if (cnt[3]) {
            ++foreign;
         } else if (kinds == 1) {
            ++solid;
            color_hist[cnt[0] ? 0 : cnt[1] ? 1 : 2]++;
         } else {
            ++mixed;
         }
         if ((cnt[3] || kinds > 1) && saved < 4) {
            char path[64];
            snprintf(path, sizeof path, "bad-%06u.ppm", seq);
            save_frame(path, maps[slot]);
            printf("BAD seq=%u r=%u g=%u b=%u other=%u\n", seq, cnt[0], cnt[1], cnt[2], cnt[3]);
            ++saved;
         }
         if (samples == 1) save_frame("first.ppm", maps[slot]);
         sync_cpu(fds[slot], DMA_BUF_SYNC_END);
      }
      uint64_t tick = begin + 8333333;
      struct timespec until = { .tv_sec = tick / 1000000000, .tv_nsec = tick % 1000000000 };
      while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &until, NULL) == EINTR) {}
   }
   printf("RESULT samples=%u solid=%u mixed=%u foreign=%u hist=%u/%u/%u elapsed_ms=%.0f\n",
          samples, solid, mixed, foreign, color_hist[0], color_hist[1], color_hist[2],
          t0 ? (now_ns() - t0) / 1e6 : 0.0);
   disconnect(ctx);
   return 0;
}
