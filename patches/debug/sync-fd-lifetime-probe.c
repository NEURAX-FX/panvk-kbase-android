#define _GNU_SOURCE
#include <vulkan/vulkan.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/dma-buf.h>
#include <linux/dma-heap.h>
#include <drm/vgem_drm.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define REQUIRE(x) do { if (!(x)) { \
   fprintf(stderr, "FAIL line %d: %s (errno=%d)\n", __LINE__, #x, errno); \
   exit(1); \
} } while (0)
#define VK_OK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { \
   fprintf(stderr, "FAIL line %d: %s returned %d\n", __LINE__, #x, r_); \
   exit(1); \
} } while (0)

static int fd_count(void)
{
   DIR *dir = opendir("/proc/self/fd");
   REQUIRE(dir);
   int count = 0;
   struct dirent *entry;
   while ((entry = readdir(dir)))
      count += entry->d_name[0] != '.';
   closedir(dir);
   return count;
}

static int export_sync(int dmabuf)
{
   struct dma_buf_export_sync_file e = { .flags = DMA_BUF_SYNC_RW, .fd = -1 };
   REQUIRE(ioctl(dmabuf, DMA_BUF_IOCTL_EXPORT_SYNC_FILE, &e) == 0);
   REQUIRE(e.fd >= 0);
   return e.fd;
}

int main(int argc, char **argv)
{
   const char *mode = argc > 1 ? argv[1] : "wait";
   const unsigned iterations = argc > 2 ? strtoul(argv[2], NULL, 10) : 256;
   const char *modes[] = {"wait", "reset", "signal", "replace", "destroy", "semaphore", "timeout"};
   int known = 0;
   for (unsigned i = 0; i < sizeof(modes) / sizeof(modes[0]); i++)
      known |= strcmp(mode, modes[i]) == 0;
   REQUIRE(known && iterations > 0);
   setvbuf(stdout, NULL, _IOLBF, 0);

   VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
      .pApplicationName = "panvk-sync-fd-lifetime", .apiVersion = VK_API_VERSION_1_2 };
   VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
      .pApplicationInfo = &app };
   VkInstance instance;
   VK_OK(vkCreateInstance(&ici, NULL, &instance));
   uint32_t count = 0;
   VK_OK(vkEnumeratePhysicalDevices(instance, &count, NULL));
   REQUIRE(count == 1);
   VkPhysicalDevice physical;
   VK_OK(vkEnumeratePhysicalDevices(instance, &count, &physical));
   VkPhysicalDeviceDriverProperties driver = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES };
   VkPhysicalDeviceProperties2 props = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &driver };
   vkGetPhysicalDeviceProperties2(physical, &props);
   printf("DEVICE %s DRIVER %s %s\n", props.properties.deviceName, driver.driverName, driver.driverInfo);
   REQUIRE(driver.driverID == VK_DRIVER_ID_MESA_PANVK);
   vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, NULL);
   VkQueueFamilyProperties *families = calloc(count, sizeof(*families));
   REQUIRE(families);
   vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, families);
   uint32_t family = 0;
   while (family < count && !(families[family].queueFlags & VK_QUEUE_GRAPHICS_BIT))
      family++;
   REQUIRE(family < count);
   free(families);
   float priority = 1.0f;
   VkDeviceQueueCreateInfo qci = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
      .queueFamilyIndex = family, .queueCount = 1, .pQueuePriorities = &priority };
   const char *extensions[] = { VK_KHR_EXTERNAL_FENCE_FD_EXTENSION_NAME,
      VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME };
   VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
      .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci,
      .enabledExtensionCount = 2, .ppEnabledExtensionNames = extensions };
   VkDevice device;
   VK_OK(vkCreateDevice(physical, &dci, NULL, &device));
   VkQueue queue;
   vkGetDeviceQueue(device, family, 0, &queue);
   PFN_vkImportFenceFdKHR import_fence = (PFN_vkImportFenceFdKHR)
      vkGetDeviceProcAddr(device, "vkImportFenceFdKHR");
   PFN_vkImportSemaphoreFdKHR import_sem = (PFN_vkImportSemaphoreFdKHR)
      vkGetDeviceProcAddr(device, "vkImportSemaphoreFdKHR");
   REQUIRE(import_fence && import_sem);
   VkSubmitInfo empty = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO };
   VK_OK(vkQueueSubmit(queue, 1, &empty, VK_NULL_HANDLE));
   VK_OK(vkQueueWaitIdle(queue));

   int heap = open("/dev/dma_heap/system", O_RDWR | O_CLOEXEC);
   REQUIRE(heap >= 0);
   struct dma_heap_allocation_data alloc = { .len = 4096,
      .fd_flags = O_RDWR | O_CLOEXEC };
   REQUIRE(ioctl(heap, DMA_HEAP_IOCTL_ALLOC, &alloc) == 0);
   int dmabuf = alloc.fd;
   close(heap);
   int vgem = -1;
   struct drm_prime_handle prime = { .fd = dmabuf };
   if (!strcmp(mode, "timeout")) {
      vgem = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
      REQUIRE(vgem >= 0);
      REQUIRE(ioctl(vgem, DRM_IOCTL_PRIME_FD_TO_HANDLE, &prime) == 0);
   }
   int before = fd_count();
   for (unsigned i = 0; i < iterations; i++) {
      struct drm_vgem_fence_attach attach = { .handle = prime.handle,
         .flags = VGEM_FENCE_WRITE };
      if (vgem >= 0)
         REQUIRE(ioctl(vgem, DRM_IOCTL_VGEM_FENCE_ATTACH, &attach) == 0);
      VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
      VkFence fence;
      VK_OK(vkCreateFence(device, &fci, NULL, &fence));
      if (!strcmp(mode, "semaphore")) {
         VkSemaphoreCreateInfo sci = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
         VkSemaphore sem;
         VK_OK(vkCreateSemaphore(device, &sci, NULL, &sem));
         VkImportSemaphoreFdInfoKHR si = { .sType = VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_FD_INFO_KHR,
            .semaphore = sem, .flags = VK_SEMAPHORE_IMPORT_TEMPORARY_BIT,
            .handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT, .fd = export_sync(dmabuf) };
         VK_OK(import_sem(device, &si));
         VkPipelineStageFlags stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
         VkSubmitInfo submit = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
            .waitSemaphoreCount = 1, .pWaitSemaphores = &sem, .pWaitDstStageMask = &stage };
         VK_OK(vkQueueSubmit(queue, 1, &submit, fence));
         VK_OK(vkWaitForFences(device, 1, &fence, VK_TRUE, 5000000000ull));
         VK_OK(vkQueueWaitIdle(queue));
         vkDestroySemaphore(device, sem, NULL);
      } else {
         VkImportFenceFdInfoKHR fi = { .sType = VK_STRUCTURE_TYPE_IMPORT_FENCE_FD_INFO_KHR,
            .fence = fence, .handleType = VK_EXTERNAL_FENCE_HANDLE_TYPE_SYNC_FD_BIT,
            .fd = export_sync(dmabuf) };
         VK_OK(import_fence(device, &fi));
         if (!strcmp(mode, "wait")) {
            VK_OK(vkWaitForFences(device, 1, &fence, VK_TRUE, 5000000000ull));
         } else if (!strcmp(mode, "reset")) {
            VK_OK(vkResetFences(device, 1, &fence));
            REQUIRE(vkGetFenceStatus(device, fence) == VK_NOT_READY);
         } else if (!strcmp(mode, "signal")) {
            fi.fd = -1;
            VK_OK(import_fence(device, &fi));
            VK_OK(vkGetFenceStatus(device, fence));
         } else if (!strcmp(mode, "replace")) {
            fi.fd = export_sync(dmabuf);
            VK_OK(import_fence(device, &fi));
         } else if (vgem >= 0) {
            REQUIRE(vkWaitForFences(device, 1, &fence, VK_TRUE, 0) == VK_TIMEOUT);
            REQUIRE(vkGetFenceStatus(device, fence) == VK_NOT_READY);
            struct drm_vgem_fence_signal signal = { .fence = attach.out_fence };
            REQUIRE(ioctl(vgem, DRM_IOCTL_VGEM_FENCE_SIGNAL, &signal) == 0);
            VK_OK(vkWaitForFences(device, 1, &fence, VK_TRUE, 5000000000ull));
         }
      }
      vkDestroyFence(device, fence, NULL);
   }
   VK_OK(vkDeviceWaitIdle(device));
   int after = fd_count();
   printf("mode=%s iterations=%u fd_before=%d fd_after=%d delta=%d\n",
          mode, iterations, before, after, after - before);
   if (vgem >= 0) {
      struct drm_gem_close gc = { .handle = prime.handle };
      REQUIRE(ioctl(vgem, DRM_IOCTL_GEM_CLOSE, &gc) == 0);
      close(vgem);
   }
   close(dmabuf);
   vkDestroyDevice(device, NULL);
   vkDestroyInstance(instance, NULL);
   if (before != after) {
      fputs("FAIL: sync payload lifecycle leaked file descriptors\n", stderr);
      return 1;
   }
   puts("PASS: sync payload lifecycle returns to the baseline fd count");
   return 0;
}
