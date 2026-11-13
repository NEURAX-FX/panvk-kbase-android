/* vgem <-> PanVK(kbase) dma-buf interop probe. Throwaway diagnostic.
 *
 * A: vgem dumb BO -> PRIME fd -> PanVK import -> GPU fill -> CPU read via vgem mmap
 * B: PanVK exportable memory -> dma-buf fd -> vgem PRIME import -> CPU read via vgem mmap
 * Also: implicit-sync ioctls (EXPORT/IMPORT_SYNC_FILE) on both dma-bufs, and
 * the exporter name (dma-buf fdinfo) so we know who allocated the pages. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <linux/dma-buf.h>
#include <xf86drm.h>
#include <drm.h>
#include <drm_mode.h>
#include <vulkan/vulkan.h>

#define W 256
#define H 256
#define SZ ((VkDeviceSize)W * H * 4)
#define CHK(x) do { VkResult r_ = (x); if (r_) { printf("FAIL %s = %d\n", #x, r_); exit(1); } } while (0)

static VkDevice dev;
static VkPhysicalDevice pd;
static VkQueue q;
static VkCommandPool pool;
static PFN_vkGetMemoryFdKHR getfd;
static PFN_vkGetMemoryFdPropertiesKHR getfdprops;

static void exporter(const char *tag, int fd)
{
   char p[64], line[256];
   snprintf(p, sizeof(p), "/proc/self/fdinfo/%d", fd);
   FILE *f = fopen(p, "r");
   while (f && fgets(line, sizeof(line), f))
      if (!strncmp(line, "exp_name", 8) || !strncmp(line, "size", 4))
         printf("%s %s", tag, line);
   if (f)
      fclose(f);
}

static void sync_file_probe(const char *tag, int fd)
{
   struct dma_buf_export_sync_file e = { .flags = DMA_BUF_SYNC_RW, .fd = -1 };
   int r = ioctl(fd, DMA_BUF_IOCTL_EXPORT_SYNC_FILE, &e);
   printf("%s EXPORT_SYNC_FILE: %s\n", tag, r ? strerror(errno) : "ok");
   if (r)
      return;
   struct dma_buf_import_sync_file i = { .flags = DMA_BUF_SYNC_RW, .fd = e.fd };
   r = ioctl(fd, DMA_BUF_IOCTL_IMPORT_SYNC_FILE, &i);
   printf("%s IMPORT_SYNC_FILE: %s\n", tag, r ? strerror(errno) : "ok");
   close(e.fd);
}

static uint32_t memtype(uint32_t bits, VkMemoryPropertyFlags want)
{
   VkPhysicalDeviceMemoryProperties mp;
   vkGetPhysicalDeviceMemoryProperties(pd, &mp);
   for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
      if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want)
         return i;
   for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
      if (bits & (1u << i))
         return i;
   return 0;
}

static void gpu_fill(VkBuffer b, uint32_t v)
{
   VkCommandBufferAllocateInfo ai = {
      VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, 0, pool,
      VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1 };
   VkCommandBuffer cb;
   CHK(vkAllocateCommandBuffers(dev, &ai, &cb));
   VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
   CHK(vkBeginCommandBuffer(cb, &bi));
   vkCmdFillBuffer(cb, b, 0, SZ, v);
   VkMemoryBarrier mb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER, 0,
                          VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT };
   vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
                        VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, 0, 0, 0);
   CHK(vkEndCommandBuffer(cb));
   VkFenceCreateInfo fi = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
   VkFence f;
   CHK(vkCreateFence(dev, &fi, 0, &f));
   VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO, 0, 0, 0, 0, 1, &cb };
   CHK(vkQueueSubmit(q, 1, &si, f));
   CHK(vkWaitForFences(dev, 1, &f, 1, ~0ull));
   vkDestroyFence(dev, f, 0);
   vkFreeCommandBuffers(dev, pool, 1, &cb);
}

static size_t count(const uint32_t *p, uint32_t v)
{
   size_t n = 0;
   for (size_t i = 0; i < SZ / 4; i++)
      n += p[i] == v;
   return n;
}

static void cpu_sync(int fd, uint64_t f)
{
   struct dma_buf_sync s = { f };
   if (ioctl(fd, DMA_BUF_IOCTL_SYNC, &s))
      printf("  DMA_BUF_IOCTL_SYNC: %s\n", strerror(errno));
}

/* After the Vulkan fence returned: how much of the buffer the CPU sees at
 * once, and how long until every word holds the GPU value. */
static void settle(const char *tag, const volatile uint32_t *p, uint32_t v)
{
   struct timespec t0, t;
   clock_gettime(CLOCK_MONOTONIC, &t0);
   size_t first = count((const uint32_t *)p, v), n = first;
   long us = 0;
   while (n != SZ / 4 && us < 1000000) {
      clock_gettime(CLOCK_MONOTONIC, &t);
      us = (t.tv_sec - t0.tv_sec) * 1000000 + (t.tv_nsec - t0.tv_nsec) / 1000;
      n = count((const uint32_t *)p, v);
   }
   printf("%s first read %zu/%llu words, complete=%d after %ld us\n", tag, first,
          (unsigned long long)(SZ / 4), n == SZ / 4, us);
}

static VkBuffer make_buffer(VkExternalMemoryHandleTypeFlags ht)
{
   VkExternalMemoryBufferCreateInfo eb = {
      VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO, 0, ht };
   VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, &eb, 0, SZ,
                              VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                 VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                              VK_SHARING_MODE_EXCLUSIVE };
   VkBuffer b;
   CHK(vkCreateBuffer(dev, &bci, 0, &b));
   return b;
}

/* vgem map of a GEM handle: MODE_MAP_DUMB works for any vgem GEM object,
 * including PRIME imports. */
static void *vgem_map(int vfd, uint32_t handle)
{
   struct drm_mode_map_dumb m = { .handle = handle };
   if (drmIoctl(vfd, DRM_IOCTL_MODE_MAP_DUMB, &m)) {
      printf("  MAP_DUMB: %s\n", strerror(errno));
      return MAP_FAILED;
   }
   void *p = mmap(0, SZ, PROT_READ | PROT_WRITE, MAP_SHARED, vfd, m.offset);
   if (p == MAP_FAILED)
      printf("  mmap vgem: %s\n", strerror(errno));
   return p;
}

static void test_a(int vfd)
{
   printf("== A: vgem allocates, PanVK renders ==\n");
   struct drm_mode_create_dumb c = { .width = W, .height = H, .bpp = 32 };
   if (drmIoctl(vfd, DRM_IOCTL_MODE_CREATE_DUMB, &c)) {
      printf("A CREATE_DUMB: %s\n", strerror(errno));
      return;
   }
   printf("A dumb handle=%u pitch=%u size=%llu\n", c.handle, c.pitch,
          (unsigned long long)c.size);
   int fd = -1;
   if (drmPrimeHandleToFD(vfd, c.handle, DRM_CLOEXEC | DRM_RDWR, &fd)) {
      printf("A PRIME export: %s\n", strerror(errno));
      return;
   }
   exporter("A", fd);
   sync_file_probe("A", fd);

   uint32_t *cpu = vgem_map(vfd, c.handle);
   if (cpu == MAP_FAILED)
      return;
   if (getenv("VGEM_MEMSET"))
      memset(cpu, 0, SZ);

   VkMemoryFdPropertiesKHR fp = { VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR };
   VkResult r = getfdprops(dev, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
                           fd, &fp);
   printf("A vkGetMemoryFdPropertiesKHR=%d typeBits=0x%x\n", r, fp.memoryTypeBits);
   if (r)
      return;
   VkBuffer b = make_buffer(VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT);
   VkMemoryRequirements mr;
   vkGetBufferMemoryRequirements(dev, b, &mr);
   int dupfd = dup(fd);
   VkImportMemoryFdInfoKHR imp = { VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR, 0,
                                   VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
                                   dupfd };
   VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &imp,
                                mr.size, memtype(mr.memoryTypeBits & fp.memoryTypeBits, 0) };
   VkDeviceMemory m;
   r = vkAllocateMemory(dev, &mai, 0, &m);
   printf("A vkAllocateMemory(import vgem dma-buf)=%d\n", r);
   if (r)
      return;
   CHK(vkBindBufferMemory(dev, b, m, 0));
   gpu_fill(b, 0xA5C3E1F0u);
   cpu_sync(fd, DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ);
   settle("A vgem mmap (WC):", cpu, 0xA5C3E1F0u);
   uint32_t *d = mmap(0, SZ, PROT_READ, MAP_SHARED, fd, 0);
   if (d != MAP_FAILED) {
      printf("A through dma-buf mmap: %zu words\n", count(d, 0xA5C3E1F0u));
      munmap(d, SZ);
   }
   cpu_sync(fd, DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ);
   vkDestroyBuffer(dev, b, 0);
   vkFreeMemory(dev, m, 0);
   munmap(cpu, SZ);
   close(fd);
}

static void test_b(int vfd)
{
   printf("== B: PanVK allocates (dma-heap), vgem imports ==\n");
   VkBuffer b = make_buffer(VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT);
   VkMemoryRequirements mr;
   vkGetBufferMemoryRequirements(dev, b, &mr);
   VkExportMemoryAllocateInfo ex = { VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO, 0,
                                     VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT };
   VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &ex, mr.size,
                                memtype(mr.memoryTypeBits, 0) };
   VkDeviceMemory m;
   CHK(vkAllocateMemory(dev, &mai, 0, &m));
   CHK(vkBindBufferMemory(dev, b, m, 0));
   VkMemoryGetFdInfoKHR gi = { VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR, 0, m,
                               VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT };
   int fd = -1;
   VkResult r = getfd(dev, &gi, &fd);
   printf("B vkGetMemoryFdKHR=%d fd=%d\n", r, fd);
   if (r)
      return;
   exporter("B", fd);
   sync_file_probe("B", fd);
   uint32_t *d = mmap(0, SZ, PROT_READ, MAP_SHARED, fd, 0);
   gpu_fill(b, 0x5A3C1E0Fu);
   if (d != MAP_FAILED) {
      cpu_sync(fd, DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ);
      settle("B dma-heap mmap + SYNC_START:", d, 0x5A3C1E0Fu);
      cpu_sync(fd, DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ);
      gpu_fill(b, 0x11223344u);
      settle("B dma-heap mmap, no SYNC_START:", d, 0x11223344u);
      munmap(d, SZ);
   }

   uint32_t h = 0;
   if (drmPrimeFDToHandle(vfd, fd, &h)) {
      printf("B vgem PRIME import: %s\n", strerror(errno));
   } else {
      printf("B vgem PRIME import handle=%u\n", h);
      uint32_t *cpu = vgem_map(vfd, h);
      if (cpu != MAP_FAILED) {
         cpu_sync(fd, DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ);
         printf("B GPU fill seen through vgem mmap: %zu/%llu words\n",
                count(cpu, 0x5A3C1E0Fu), (unsigned long long)(SZ / 4));
         cpu_sync(fd, DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ);
         munmap(cpu, SZ);
      }
   }
   close(fd);
   vkDestroyBuffer(dev, b, 0);
   vkFreeMemory(dev, m, 0);
}

int main(void)
{
   const char *node = getenv("VGEM_NODE") ? getenv("VGEM_NODE") : "/dev/dri/card1";
   int vfd = open(node, O_RDWR | O_CLOEXEC);
   if (vfd < 0) {
      printf("open %s: %s\n", node, strerror(errno));
      return 1;
   }
   drmVersionPtr v = drmGetVersion(vfd);
   printf("node %s driver=%s\n", node, v ? v->name : "?");
   drmFreeVersion(v);

   VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO, 0, "vgem-probe", 1,
                             0, 0, VK_API_VERSION_1_1 };
   VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, 0, 0, &app };
   VkInstance inst;
   CHK(vkCreateInstance(&ici, 0, &inst));
   uint32_t n = 1;
   vkEnumeratePhysicalDevices(inst, &n, &pd);
   VkPhysicalDeviceProperties pp;
   vkGetPhysicalDeviceProperties(pd, &pp);
   printf("vk device %s\n", pp.deviceName);
   float pr = 1;
   VkDeviceQueueCreateInfo qci = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, 0, 0, 0, 1, &pr };
   const char *exts[] = { "VK_KHR_external_memory_fd", "VK_EXT_external_memory_dma_buf" };
   VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, 0, 0, 1, &qci, 0, 0, 2, exts };
   CHK(vkCreateDevice(pd, &dci, 0, &dev));
   vkGetDeviceQueue(dev, 0, 0, &q);
   VkCommandPoolCreateInfo pci = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, 0,
                                   VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, 0 };
   CHK(vkCreateCommandPool(dev, &pci, 0, &pool));
   getfd = (PFN_vkGetMemoryFdKHR)vkGetDeviceProcAddr(dev, "vkGetMemoryFdKHR");
   getfdprops = (PFN_vkGetMemoryFdPropertiesKHR)vkGetDeviceProcAddr(dev, "vkGetMemoryFdPropertiesKHR");

   test_a(vfd);
   test_b(vfd);

   vkDestroyCommandPool(dev, pool, 0);
   vkDestroyDevice(dev, 0);
   vkDestroyInstance(inst, 0);
   close(vfd);
   return 0;
}
