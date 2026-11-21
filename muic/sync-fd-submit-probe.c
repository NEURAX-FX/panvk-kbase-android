/*
 * Real PanVK/vgem pending-SYNC_FD submission regression; no mocked fences.
 *
 * Build (use the corresponding target sysroot/pkg-config when cross compiling):
 *   cc -std=c11 -O2 -Wall -Wextra -Werror -pthread \
 *      sync-fd-submit-probe.c -o sync-fd-submit-probe \
 *      $(pkg-config --cflags --libs vulkan)
 * Requires Vulkan headers/loader, Linux dma-buf/dma-heap/sync_file headers and
 * <drm/vgem_drm.h> (add the directory containing drm/ with -I if necessary).
 * libdrm itself is not linked. Select the intended PanVK ICD in the environment.
 *
 * Usage: sync-fd-submit-probe <mode> [iterations]
 *   all       Run every mode (default iterations: 128 per mode).
 *   empty     Pending imported wait, no command buffer, fence + output SYNC_FD.
 *   command   Pending imported wait plus vkCmdFillBuffer and host readback.
 *   multi     Three distinct pending waits submitted separately to one queue.
 *   followup  Wait-only submission followed by a separate fill submission.
 *   queues    Two queues with independent gates; SKIP if queueCount < 2.
 *
 * Each mode executes two uncounted full warmups before its fd-count baseline.
 * Every measured iteration creates/destroys its gates and Vulkan objects and
 * must return to that baseline. No per-iteration sleeps: an explicit vgem
 * signal releases work after the pending checks. A condition-variable helper
 * releases outstanding gates after 2 seconds to rescue the old CPU-polling
 * driver, but that release ALWAYS makes the test FAIL. A supervisor process
 * additionally kills/reaps a stuck worker after 20 seconds without progress,
 * including failures during initialization/cleanup. It owns no Vulkan objects.
 */
#define _GNU_SOURCE
#include <vulkan/vulkan.h>

#include <dirent.h>
#include <drm/vgem_drm.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <linux/dma-buf.h>
#include <linux/dma-heap.h>
#include <linux/sync_file.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/prctl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define MAX_JOBS 3u
#define BUFFER_BYTES 4096u
#define RELEASE_MS 2000
#define WATCHDOG_MS 20000
#define COMPLETE_NS UINT64_C(5000000000)

static const char *current_mode = "setup";
static unsigned current_iteration;
static bool warmup;

static bool
fail(const char *format, ...)
{
   va_list ap;
   fprintf(stderr, "FAIL mode=%s %s=%u: ", current_mode,
           warmup ? "warmup" : "iteration", current_iteration);
   va_start(ap, format);
   vfprintf(stderr, format, ap);
   va_end(ap);
   fputc('\n', stderr);
   return false;
}

#define REQUIRE(test, ...) do { if (!(test)) return fail(__VA_ARGS__); } while (0)
#define SYSTEM(test) do { if (!(test)) \
   return fail("%s: %s", #test, strerror(errno)); } while (0)
#define VK_CHECK(call) do { VkResult result_ = (call); \
   if (result_ != VK_SUCCESS) \
      return fail("%s returned VkResult %d", #call, (int)result_); \
} while (0)

struct fixture {
   VkInstance instance;
   VkPhysicalDevice physical;
   VkDevice device;
   VkQueue queues[2];
   uint32_t family;
   uint32_t queue_count;
   VkCommandPool pool;
   VkPhysicalDeviceMemoryProperties memory;
   PFN_vkImportSemaphoreFdKHR import_sem;
   PFN_vkGetSemaphoreFdKHR export_sem;
   int vgem;
};

struct gate {
   int dmabuf;
   int sync_fd;                 /* Retained observation fd, not given to Vulkan. */
   uint32_t handle;
   uint32_t fence;
   bool has_handle;
   bool pending;
   VkSemaphore semaphore;
};

struct job {
   VkFence fence;
   VkSemaphore output;
   int output_fd;
   VkBuffer buffer;
   VkDeviceMemory memory;
   void *mapping;
   VkCommandBuffer command;
   uint32_t value;
   unsigned queue;
   int wait_gate;               /* -1 for the followup command. */
   bool commands;
   bool submitted;
};

struct release_guard {
   pthread_t thread;
   pthread_mutex_t mutex;
   pthread_cond_t condition;
   struct timespec deadline;
   struct gate *gates;
   unsigned count;
   int vgem;
   int error;
   bool timed_out;
   bool stop;
   bool started;
};

struct iteration {
   struct gate gates[MAX_JOBS];
   struct job jobs[MAX_JOBS];
   unsigned gate_count;
   unsigned job_count;
   struct release_guard guard;
};

static int
fd_count(void)
{
   DIR *dir = opendir("/proc/self/fd");
   if (!dir) {
      fail("opendir(/proc/self/fd): %s", strerror(errno));
      return -1;
   }
   int count = 0;
   struct dirent *entry;
   errno = 0;
   while ((entry = readdir(dir)))
      count += entry->d_name[0] != '.';
   int saved = errno;
   if (closedir(dir) && !saved)
      saved = errno;
   if (saved) {
      fail("counting /proc/self/fd: %s", strerror(saved));
      return -1;
   }
   return count;
}

static bool
sync_state(int fd, bool ready, const char *name)
{
   REQUIRE(fd >= 0, "%s unexpectedly exported fd=-1 (already signalled)", name);
   struct pollfd pfd = { .fd = fd, .events = POLLIN };
   int ret;
   do {
      ret = poll(&pfd, 1, 0);
   } while (ret < 0 && errno == EINTR);
   SYSTEM(ret >= 0);
   REQUIRE(!(pfd.revents & (POLLERR | POLLHUP | POLLNVAL)),
           "%s poll error revents=0x%x", name, (unsigned)pfd.revents);
   REQUIRE(!!(pfd.revents & POLLIN) == ready,
           "%s poll: expected %s, got revents=0x%x", name,
           ready ? "signalled" : "pending", (unsigned)pfd.revents);
   struct sync_file_info info = { 0 };
   SYSTEM(ioctl(fd, SYNC_IOC_FILE_INFO, &info) == 0);
   REQUIRE(info.status == (ready ? 1 : 0),
           "%s SYNC_IOC_FILE_INFO status=%d, expected %d", name,
           (int)info.status, ready ? 1 : 0);
   return true;
}

static bool
wait_sync(int fd)
{
   struct timespec start, now;
   SYSTEM(clock_gettime(CLOCK_MONOTONIC, &start) == 0);
   int remaining = (int)(COMPLETE_NS / UINT64_C(1000000));
   struct pollfd pfd = { .fd = fd, .events = POLLIN };
   for (;;) {
      int ret = poll(&pfd, 1, remaining);
      if (ret >= 0) {
         REQUIRE(ret == 1, "output SYNC_FD did not signal within 5 seconds");
         return sync_state(fd, true, "completed output SYNC_FD");
      }
      SYSTEM(errno == EINTR);
      SYSTEM(clock_gettime(CLOCK_MONOTONIC, &now) == 0);
      int64_t elapsed = (int64_t)(now.tv_sec - start.tv_sec) * 1000 +
                        (now.tv_nsec - start.tv_nsec) / 1000000;
      REQUIRE(elapsed < (int64_t)(COMPLETE_NS / UINT64_C(1000000)),
              "output SYNC_FD wait interrupted past deadline");
      remaining = (int)(COMPLETE_NS / UINT64_C(1000000)) - (int)elapsed;
   }
}

static bool
select_physical(struct fixture *f)
{
   uint32_t count = 0;
   VK_CHECK(vkEnumeratePhysicalDevices(f->instance, &count, NULL));
   REQUIRE(count, "no Vulkan physical devices");
   VkPhysicalDevice *devices = calloc(count, sizeof(*devices));
   REQUIRE(devices, "allocating physical-device list");
   VkResult ret = vkEnumeratePhysicalDevices(f->instance, &count, devices);
   if (ret != VK_SUCCESS) {
      free(devices);
      return fail("vkEnumeratePhysicalDevices returned %d", (int)ret);
   }
   for (uint32_t i = 0; i < count; i++) {
      VkPhysicalDeviceDriverProperties driver = {
         .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES,
      };
      VkPhysicalDeviceProperties2 props = {
         .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
         .pNext = &driver,
      };
      vkGetPhysicalDeviceProperties2(devices[i], &props);
      if (driver.driverID != VK_DRIVER_ID_MESA_PANVK)
         continue;
      f->physical = devices[i];
      printf("DEVICE %s DRIVER %s %s\n", props.properties.deviceName,
             driver.driverName, driver.driverInfo);
      break;
   }
   free(devices);
   REQUIRE(f->physical != VK_NULL_HANDLE, "no Mesa PanVK physical device found");
   return true;
}

static bool
select_family(struct fixture *f)
{
   uint32_t count = 0;
   vkGetPhysicalDeviceQueueFamilyProperties(f->physical, &count, NULL);
   REQUIRE(count, "no queue families");
   VkQueueFamilyProperties *families = calloc(count, sizeof(*families));
   REQUIRE(families, "allocating queue-family list");
   vkGetPhysicalDeviceQueueFamilyProperties(f->physical, &count, families);
   f->family = UINT32_MAX;
   for (uint32_t i = 0; i < count; i++) {
      if (!(families[i].queueFlags & (VK_QUEUE_GRAPHICS_BIT |
                                     VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT)) ||
          !families[i].queueCount)
         continue;
      if (f->family == UINT32_MAX ||
          families[i].queueCount > families[f->family].queueCount)
         f->family = i;
   }
   if (f->family != UINT32_MAX)
      f->queue_count = families[f->family].queueCount >= 2 ? 2 : 1;
   free(families);
   REQUIRE(f->family != UINT32_MAX, "no transfer-capable queue family");
   return true;
}

static bool
fixture_init(struct fixture *f)
{
   VkApplicationInfo app = {
      .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
      .pApplicationName = "panvk-sync-fd-submit-probe",
      .apiVersion = VK_API_VERSION_1_2,
   };
   VkInstanceCreateInfo ici = {
      .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
      .pApplicationInfo = &app,
   };
   VK_CHECK(vkCreateInstance(&ici, NULL, &f->instance));
   if (!select_physical(f) || !select_family(f))
      return false;
   VkPhysicalDeviceExternalSemaphoreInfo external_info = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_SEMAPHORE_INFO,
      .handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT,
   };
   VkExternalSemaphoreProperties external = {
      .sType = VK_STRUCTURE_TYPE_EXTERNAL_SEMAPHORE_PROPERTIES,
   };
   vkGetPhysicalDeviceExternalSemaphoreProperties(f->physical, &external_info,
                                                 &external);
   VkExternalSemaphoreFeatureFlags required =
      VK_EXTERNAL_SEMAPHORE_FEATURE_IMPORTABLE_BIT |
      VK_EXTERNAL_SEMAPHORE_FEATURE_EXPORTABLE_BIT;
   REQUIRE((external.externalSemaphoreFeatures & required) == required,
           "SYNC_FD binary semaphore import/export not supported (features=0x%x)",
           (unsigned)external.externalSemaphoreFeatures);
   const float priorities[2] = { 1.0f, 1.0f };
   VkDeviceQueueCreateInfo qci = {
      .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
      .queueFamilyIndex = f->family,
      .queueCount = f->queue_count,
      .pQueuePriorities = priorities,
   };
   const char *extension = VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME;
   VkDeviceCreateInfo dci = {
      .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
      .queueCreateInfoCount = 1,
      .pQueueCreateInfos = &qci,
      .enabledExtensionCount = 1,
      .ppEnabledExtensionNames = &extension,
   };
   VK_CHECK(vkCreateDevice(f->physical, &dci, NULL, &f->device));
   for (uint32_t i = 0; i < f->queue_count; i++)
      vkGetDeviceQueue(f->device, f->family, i, &f->queues[i]);
   f->import_sem = (PFN_vkImportSemaphoreFdKHR)
      vkGetDeviceProcAddr(f->device, "vkImportSemaphoreFdKHR");
   f->export_sem = (PFN_vkGetSemaphoreFdKHR)
      vkGetDeviceProcAddr(f->device, "vkGetSemaphoreFdKHR");
   REQUIRE(f->import_sem && f->export_sem, "missing external semaphore entrypoints");
   vkGetPhysicalDeviceMemoryProperties(f->physical, &f->memory);
   VkCommandPoolCreateInfo pci = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
      .queueFamilyIndex = f->family,
   };
   VK_CHECK(vkCreateCommandPool(f->device, &pci, NULL, &f->pool));
   f->vgem = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
   SYSTEM(f->vgem >= 0);
   char name[16] = { 0 };
   struct drm_version version = { .name_len = sizeof(name) - 1, .name = name };
   SYSTEM(ioctl(f->vgem, DRM_IOCTL_VERSION, &version) == 0);
   REQUIRE(!strcmp(name, "vgem"), "/dev/dri/renderD128 is %s, not vgem", name);
   printf("QUEUE family=%u usable_count=%u\n", f->family, f->queue_count);
   return true;
}

static void
fixture_finish(struct fixture *f)
{
   if (f->vgem >= 0)
      close(f->vgem);
   if (f->pool)
      vkDestroyCommandPool(f->device, f->pool, NULL);
   if (f->device)
      vkDestroyDevice(f->device, NULL);
   if (f->instance)
      vkDestroyInstance(f->instance, NULL);
}

static bool
gate_create(struct fixture *f, struct gate *g)
{
   int heap = open("/dev/dma_heap/system", O_RDWR | O_CLOEXEC);
   SYSTEM(heap >= 0);
   struct dma_heap_allocation_data alloc = {
      .len = BUFFER_BYTES, .fd_flags = O_RDWR | O_CLOEXEC,
   };
   int ret = ioctl(heap, DMA_HEAP_IOCTL_ALLOC, &alloc);
   int saved = errno;
   close(heap);
   if (ret)
      return fail("DMA_HEAP_IOCTL_ALLOC: %s", strerror(saved));
   g->dmabuf = (int)alloc.fd;
   struct drm_prime_handle prime = { .fd = g->dmabuf };
   SYSTEM(ioctl(f->vgem, DRM_IOCTL_PRIME_FD_TO_HANDLE, &prime) == 0);
   g->handle = prime.handle;
   g->has_handle = true;
   struct drm_vgem_fence_attach attach = {
      .handle = g->handle, .flags = VGEM_FENCE_WRITE,
   };
   SYSTEM(ioctl(f->vgem, DRM_IOCTL_VGEM_FENCE_ATTACH, &attach) == 0);
   g->fence = attach.out_fence;
   g->pending = true;
   struct dma_buf_export_sync_file export = { .flags = DMA_BUF_SYNC_RW, .fd = -1 };
   SYSTEM(ioctl(g->dmabuf, DMA_BUF_IOCTL_EXPORT_SYNC_FILE, &export) == 0);
   g->sync_fd = export.fd;
   if (!sync_state(g->sync_fd, false, "vgem input before import"))
      return false;
   VkSemaphoreCreateInfo sci = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
   VK_CHECK(vkCreateSemaphore(f->device, &sci, NULL, &g->semaphore));
   int imported_fd = fcntl(g->sync_fd, F_DUPFD_CLOEXEC, 0);
   SYSTEM(imported_fd >= 0);
   VkImportSemaphoreFdInfoKHR import = {
      .sType = VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_FD_INFO_KHR,
      .semaphore = g->semaphore,
      .flags = VK_SEMAPHORE_IMPORT_TEMPORARY_BIT,
      .handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT,
      .fd = imported_fd,
   };
   VkResult result = f->import_sem(f->device, &import);
   if (result != VK_SUCCESS) {
      close(imported_fd); /* Ownership transfers only on successful import. */
      return fail("vkImportSemaphoreFdKHR returned %d", (int)result);
   }
   return sync_state(g->sync_fd, false, "vgem input after temporary import");
}

static int
signal_gate(int vgem, struct gate *g)
{
   if (!g->pending)
      return 0;
   struct drm_vgem_fence_signal signal = { .fence = g->fence };
   if (ioctl(vgem, DRM_IOCTL_VGEM_FENCE_SIGNAL, &signal))
      return errno;
   g->pending = false;
   return 0;
}

static bool
any_pending(const struct release_guard *g)
{
   for (unsigned i = 0; i < g->count; i++) {
      if (g->gates[i].pending)
         return true;
   }
   return false;
}

static void *
release_thread(void *data)
{
   struct release_guard *g = data;
   pthread_mutex_lock(&g->mutex);
   while (!g->stop && any_pending(g)) {
      int ret = pthread_cond_timedwait(&g->condition, &g->mutex, &g->deadline);
      if (ret == ETIMEDOUT && any_pending(g)) {
         g->timed_out = true;
         break;
      }
      if (ret && ret != ETIMEDOUT) {
         g->error = ret;
         break;
      }
   }
   /* Also release all gates on failure cleanup, before joining/destroying Vulkan. */
   for (unsigned i = 0; i < g->count; i++) {
      int ret = signal_gate(g->vgem, &g->gates[i]);
      if (ret && !g->error)
         g->error = ret;
   }
   pthread_mutex_unlock(&g->mutex);
   return NULL;
}

static bool
guard_start(struct release_guard *g, struct fixture *f, struct iteration *it)
{
   g->gates = it->gates;
   g->count = it->gate_count;
   g->vgem = f->vgem;
   SYSTEM(clock_gettime(CLOCK_MONOTONIC, &g->deadline) == 0);
   g->deadline.tv_sec += RELEASE_MS / 1000;
   int ret = pthread_mutex_init(&g->mutex, NULL);
   REQUIRE(!ret, "pthread_mutex_init: %s", strerror(ret));
   pthread_condattr_t attr;
   ret = pthread_condattr_init(&attr);
   if (ret) {
      pthread_mutex_destroy(&g->mutex);
      return fail("pthread_condattr_init: %s", strerror(ret));
   }
   ret = pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
   if (!ret)
      ret = pthread_cond_init(&g->condition, &attr);
   pthread_condattr_destroy(&attr);
   if (ret) {
      pthread_mutex_destroy(&g->mutex);
      return fail("initializing monotonic condition variable: %s", strerror(ret));
   }
   ret = pthread_create(&g->thread, NULL, release_thread, g);
   if (ret) {
      pthread_cond_destroy(&g->condition);
      pthread_mutex_destroy(&g->mutex);
      return fail("pthread_create: %s", strerror(ret));
   }
   g->started = true;
   return true;
}

static bool
guard_check(struct release_guard *g)
{
   pthread_mutex_lock(&g->mutex);
   bool timed_out = g->timed_out;
   int error = g->error;
   pthread_mutex_unlock(&g->mutex);
   REQUIRE(!timed_out,
           "watchdog had to release vgem after %d ms; submission/export blocked on CPU",
           RELEASE_MS);
   REQUIRE(!error, "release helper: %s", strerror(error));
   return true;
}

static bool
guard_signal(struct release_guard *g, unsigned index)
{
   if (!guard_check(g))
      return false;
   pthread_mutex_lock(&g->mutex);
   int error = g->timed_out ? ETIMEDOUT : signal_gate(g->vgem, &g->gates[index]);
   pthread_cond_signal(&g->condition);
   pthread_mutex_unlock(&g->mutex);
   REQUIRE(!error, "explicit vgem signal %u: %s", index, strerror(error));
   return guard_check(g) && sync_state(g->gates[index].sync_fd, true,
                                       "explicitly signalled vgem input");
}

static bool
guard_finish(struct release_guard *g)
{
   if (!g->started)
      return true;
   pthread_mutex_lock(&g->mutex);
   g->stop = true;
   pthread_cond_signal(&g->condition);
   pthread_mutex_unlock(&g->mutex);
   int ret = pthread_join(g->thread, NULL);
   /* A valid created, joinable thread has no recoverable join failure. */
   if (ret) {
      fail("pthread_join: %s", strerror(ret));
      _exit(1); /* Terminate helper too; supervisor reaps this worker. */
   }
   bool ok = guard_check(g);
   pthread_cond_destroy(&g->condition);
   pthread_mutex_destroy(&g->mutex);
   g->started = false;
   return ok;
}

static bool
buffer_create(struct fixture *f, struct job *j)
{
   VkBufferCreateInfo bci = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
      .size = BUFFER_BYTES,
      .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
   };
   VK_CHECK(vkCreateBuffer(f->device, &bci, NULL, &j->buffer));
   VkMemoryRequirements requirements;
   vkGetBufferMemoryRequirements(f->device, j->buffer, &requirements);
   uint32_t type = UINT32_MAX;
   for (uint32_t i = 0; i < f->memory.memoryTypeCount; i++) {
      if (!(requirements.memoryTypeBits & (1u << i)) ||
          !(f->memory.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT))
         continue;
      type = i;
      if (f->memory.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)
         break;
   }
   REQUIRE(type != UINT32_MAX, "fill buffer has no HOST_VISIBLE memory type");
   VkMemoryAllocateInfo mai = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = requirements.size,
      .memoryTypeIndex = type,
   };
   VK_CHECK(vkAllocateMemory(f->device, &mai, NULL, &j->memory));
   VK_CHECK(vkBindBufferMemory(f->device, j->buffer, j->memory, 0));
   VK_CHECK(vkMapMemory(f->device, j->memory, 0, VK_WHOLE_SIZE, 0, &j->mapping));
   memset(j->mapping, 0xa5, BUFFER_BYTES);
   VkMappedMemoryRange range = {
      .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
      .memory = j->memory, .size = VK_WHOLE_SIZE,
   };
   VK_CHECK(vkFlushMappedMemoryRanges(f->device, 1, &range));
   VkCommandBufferAllocateInfo cai = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
      .commandPool = f->pool,
      .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
      .commandBufferCount = 1,
   };
   VK_CHECK(vkAllocateCommandBuffers(f->device, &cai, &j->command));
   VkCommandBufferBeginInfo begin = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
      .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
   };
   VK_CHECK(vkBeginCommandBuffer(j->command, &begin));
   vkCmdFillBuffer(j->command, j->buffer, 0, BUFFER_BYTES, j->value);
   VkBufferMemoryBarrier barrier = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
      .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
      .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
      .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .buffer = j->buffer, .size = VK_WHOLE_SIZE,
   };
   vkCmdPipelineBarrier(j->command, VK_PIPELINE_STAGE_TRANSFER_BIT,
                        VK_PIPELINE_STAGE_HOST_BIT, 0, 0, NULL, 1, &barrier, 0, NULL);
   VK_CHECK(vkEndCommandBuffer(j->command));
   return true;
}

static bool
job_create(struct fixture *f, struct job *j)
{
   VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
   VK_CHECK(vkCreateFence(f->device, &fci, NULL, &j->fence));
   VkExportSemaphoreCreateInfo export = {
      .sType = VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO,
      .handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT,
   };
   VkSemaphoreCreateInfo sci = {
      .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, .pNext = &export,
   };
   VK_CHECK(vkCreateSemaphore(f->device, &sci, NULL, &j->output));
   return !j->commands || buffer_create(f, j);
}

static bool
pending_job(struct fixture *f, struct job *j)
{
   VkResult ret = vkGetFenceStatus(f->device, j->fence);
   REQUIRE(ret == VK_NOT_READY, "pending submit fence status=%d (expected NOT_READY)",
           (int)ret);
   ret = vkWaitForFences(f->device, 1, &j->fence, VK_TRUE, 0);
   REQUIRE(ret == VK_TIMEOUT, "pending submit zero-time fence wait=%d", (int)ret);
   return sync_state(j->output_fd, false, "pending exported output SYNC_FD");
}

static bool
submit_job(struct fixture *f, struct iteration *it, struct job *j)
{
   VkSemaphore wait = j->wait_gate >= 0 ?
      it->gates[(unsigned)j->wait_gate].semaphore : VK_NULL_HANDLE;
   VkPipelineStageFlags stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
   VkSubmitInfo submit = {
      .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
      .waitSemaphoreCount = wait ? 1u : 0u,
      .pWaitSemaphores = wait ? &wait : NULL,
      .pWaitDstStageMask = wait ? &stage : NULL,
      .commandBufferCount = j->commands ? 1u : 0u,
      .pCommandBuffers = j->commands ? &j->command : NULL,
      .signalSemaphoreCount = 1,
      .pSignalSemaphores = &j->output,
   };
   struct timespec before, after;
   SYSTEM(clock_gettime(CLOCK_MONOTONIC, &before) == 0);
   VkResult result = vkQueueSubmit(f->queues[j->queue], 1, &submit, j->fence);
   if (result == VK_SUCCESS)
      j->submitted = true;
   SYSTEM(clock_gettime(CLOCK_MONOTONIC, &after) == 0);
   if (!guard_check(&it->guard))
      return false;
   int64_t elapsed_ns = (int64_t)(after.tv_sec - before.tv_sec) * 1000000000 +
                        after.tv_nsec - before.tv_nsec;
   REQUIRE(elapsed_ns < (int64_t)RELEASE_MS * 1000000,
           "vkQueueSubmit took %.3f ms; rescue deadline was %d ms",
           (double)elapsed_ns / 1000000.0, RELEASE_MS);
   REQUIRE(result == VK_SUCCESS, "vkQueueSubmit returned %d", (int)result);
   /* Observe the original input too: passing after an implicit/timeout signal
    * would otherwise turn an accidentally signalled-fd test into a false PASS. */
   for (unsigned i = 0; i < it->gate_count; i++) {
      if (!sync_state(it->gates[i].sync_fd, false, "vgem input after submit"))
         return false;
   }
   VkSemaphoreGetFdInfoKHR export = {
      .sType = VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR,
      .semaphore = j->output,
      .handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT,
   };
   result = f->export_sem(f->device, &export, &j->output_fd);
   if (!guard_check(&it->guard))
      return false;
   REQUIRE(result == VK_SUCCESS, "export of pending output returned %d", (int)result);
   return pending_job(f, j);
}

static bool
complete_job(struct fixture *f, struct job *j)
{
   if (!wait_sync(j->output_fd))
      return false;
   VK_CHECK(vkWaitForFences(f->device, 1, &j->fence, VK_TRUE, COMPLETE_NS));
   VK_CHECK(vkGetFenceStatus(f->device, j->fence));
   if (j->commands) {
      VkMappedMemoryRange range = {
         .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
         .memory = j->memory, .size = VK_WHOLE_SIZE,
      };
      VK_CHECK(vkInvalidateMappedMemoryRanges(f->device, 1, &range));
      const uint32_t *words = j->mapping;
      for (unsigned i = 0; i < BUFFER_BYTES / sizeof(*words); i++) {
         REQUIRE(words[i] == j->value,
                 "fill readback word %u: got 0x%08" PRIx32 ", expected 0x%08" PRIx32,
                 i, words[i], j->value);
      }
   }
   return true;
}

static bool
iteration_finish(struct fixture *f, struct iteration *it)
{
   bool ok = guard_finish(&it->guard);
   /* If setup failed before helper creation, the partially built gates still
    * belong to us and must be released before any potentially blocking teardown. */
   for (unsigned i = 0; i < it->gate_count; i++) {
      int error = signal_gate(f->vgem, &it->gates[i]);
      if (error) {
         fail("cleanup vgem signal: %s", strerror(error));
         ok = false;
      }
   }
   bool submitted = false;
   for (unsigned i = 0; i < it->job_count; i++)
      submitted |= it->jobs[i].submitted;
   if (submitted) {
      VkResult ret = vkDeviceWaitIdle(f->device);
      if (ret != VK_SUCCESS) {
         fail("cleanup vkDeviceWaitIdle returned %d", (int)ret);
         ok = false;
      }
   }
   for (unsigned i = 0; i < it->job_count; i++) {
      struct job *j = &it->jobs[i];
      if (j->output_fd >= 0)
         close(j->output_fd);
      if (j->command)
         vkFreeCommandBuffers(f->device, f->pool, 1, &j->command);
      if (j->mapping)
         vkUnmapMemory(f->device, j->memory);
      if (j->buffer)
         vkDestroyBuffer(f->device, j->buffer, NULL);
      if (j->memory)
         vkFreeMemory(f->device, j->memory, NULL);
      if (j->output)
         vkDestroySemaphore(f->device, j->output, NULL);
      if (j->fence)
         vkDestroyFence(f->device, j->fence, NULL);
   }
   for (unsigned i = 0; i < it->gate_count; i++) {
      struct gate *g = &it->gates[i];
      if (g->semaphore)
         vkDestroySemaphore(f->device, g->semaphore, NULL);
      if (g->sync_fd >= 0)
         close(g->sync_fd);
      if (g->has_handle) {
         struct drm_gem_close close_handle = { .handle = g->handle };
         if (ioctl(f->vgem, DRM_IOCTL_GEM_CLOSE, &close_handle)) {
            fail("DRM_IOCTL_GEM_CLOSE: %s", strerror(errno));
            ok = false;
         }
      }
      if (g->dmabuf >= 0)
         close(g->dmabuf);
   }
   return ok;
}

static bool
exercise(struct fixture *f, struct iteration *it)
{
   for (unsigned i = 0; i < it->gate_count; i++) {
      if (!gate_create(f, &it->gates[i]))
         return false;
   }
   for (unsigned i = 0; i < it->job_count; i++) {
      if (!job_create(f, &it->jobs[i]))
         return false;
   }
   if (!guard_start(&it->guard, f, it))
      return false;
   for (unsigned i = 0; i < it->job_count; i++) {
      if (!submit_job(f, it, &it->jobs[i]))
         return false;
   }
   /* All submissions/exports must have returned with every input still pending. */
   for (unsigned i = 0; i < it->job_count; i++) {
      if (!pending_job(f, &it->jobs[i]))
         return false;
   }
   if (!guard_check(&it->guard))
      return false;
   if (!strcmp(current_mode, "queues")) {
      /* Queue 1 must make progress without releasing queue 0's unrelated gate. */
      if (!guard_signal(&it->guard, 1) || !complete_job(f, &it->jobs[1]) ||
          !sync_state(it->gates[0].sync_fd, false, "other queue's pending input") ||
          !pending_job(f, &it->jobs[0]) || !guard_check(&it->guard) ||
          !guard_signal(&it->guard, 0) || !complete_job(f, &it->jobs[0]))
         return false;
   } else {
      for (unsigned i = 0; i < it->gate_count; i++) {
         if (!guard_signal(&it->guard, i) || !complete_job(f, &it->jobs[i]))
            return false;
         for (unsigned j = i + 1; j < it->gate_count; j++) {
            if (!sync_state(it->gates[j].sync_fd, false, "remaining pending vgem input") ||
                !pending_job(f, &it->jobs[j]))
               return false;
         }
      }
      if (!strcmp(current_mode, "followup") && !complete_job(f, &it->jobs[1]))
         return false;
   }
   return guard_check(&it->guard);
}

static bool
run_iteration(struct fixture *f)
{
   struct iteration it = { 0 };
   it.gate_count = !strcmp(current_mode, "multi") ? 3u :
                   !strcmp(current_mode, "queues") ? 2u : 1u;
   it.job_count = !strcmp(current_mode, "followup") ? 2u : it.gate_count;
   for (unsigned i = 0; i < MAX_JOBS; i++) {
      it.gates[i].dmabuf = -1;
      it.gates[i].sync_fd = -1;
      it.jobs[i].output_fd = -1;
      it.jobs[i].wait_gate = (int)i;
      it.jobs[i].commands = strcmp(current_mode, "empty") != 0;
      it.jobs[i].value = UINT32_C(0x12340000) ^ (current_iteration * 17u + i);
   }
   if (!strcmp(current_mode, "followup")) {
      it.jobs[0].commands = false;
      it.jobs[1].wait_gate = -1;
   }
   if (!strcmp(current_mode, "queues"))
      it.jobs[1].queue = 1;
   bool ok = exercise(f, &it);
   bool cleaned = iteration_finish(f, &it);
   return ok && cleaned;
}

static void
heartbeat(int fd)
{
   const char byte = '.';
   ssize_t ret;
   do {
      ret = write(fd, &byte, 1);
   } while (ret < 0 && errno == EINTR);
   /* A nonblocking pipe can be full only while the supervisor is still alive;
    * the existing bytes already provide its wakeup. */
}

static bool
run_mode(struct fixture *f, const char *mode, unsigned iterations, int progress)
{
   current_mode = mode;
   if (!strcmp(mode, "queues") && f->queue_count < 2) {
      printf("SKIP mode=queues: selected family exposes only one VkQueue\n");
      return true;
   }
   warmup = true;
   for (current_iteration = 0; current_iteration < 2; current_iteration++) {
      if (!run_iteration(f))
         return false;
      heartbeat(progress);
   }
   warmup = false;
   int baseline = fd_count();
   if (baseline < 0)
      return false;
   for (current_iteration = 0; current_iteration < iterations; current_iteration++) {
      if (!run_iteration(f))
         return false;
      int after = fd_count();
      if (after < 0)
         return false;
      REQUIRE(after == baseline,
              "fd leak after drained iteration: baseline=%d current=%d delta=%d",
              baseline, after, after - baseline);
      heartbeat(progress);
   }
   printf("PASS mode=%s iterations=%u fd_baseline=%d fd_after=%d\n",
          mode, iterations, baseline, baseline);
   return true;
}

static int
worker(const char *mode, unsigned iterations, int progress)
{
   struct fixture f = { .vgem = -1 };
   bool ok = fixture_init(&f);
   const char *modes[] = { "empty", "command", "multi", "followup", "queues" };
   if (ok) {
      for (unsigned i = 0; i < sizeof(modes) / sizeof(modes[0]); i++) {
         if (strcmp(mode, "all") && strcmp(mode, modes[i]))
            continue;
         if (!run_mode(&f, modes[i], iterations, progress)) {
            ok = false;
            break;
         }
      }
   }
   fixture_finish(&f);
   return ok ? 0 : 1;
}

static void
kill_and_reap(pid_t child)
{
   if (kill(child, SIGKILL) && errno != ESRCH)
      fprintf(stderr, "FAIL killing stuck worker: %s\n", strerror(errno));
   int status;
   while (waitpid(child, &status, 0) < 0 && errno == EINTR)
      ;
}

static int
supervise(const char *mode, unsigned iterations)
{
   int progress[2];
   if (pipe2(progress, O_CLOEXEC | O_NONBLOCK)) {
      fail("progress pipe: %s", strerror(errno));
      return 1;
   }
   pid_t parent = getpid();
   pid_t child = fork();
   if (child < 0) {
      fail("fork: %s", strerror(errno));
      close(progress[0]);
      close(progress[1]);
      return 1;
   }
   if (!child) {
      close(progress[0]);
      if (prctl(PR_SET_PDEATHSIG, SIGKILL) || getppid() != parent)
         _exit(1);
      int ret = worker(mode, iterations, progress[1]);
      fflush(NULL);
      _exit(ret);
   }
   close(progress[1]);
   struct pollfd pfd = { .fd = progress[0], .events = POLLIN };
   for (;;) {
      int ret = poll(&pfd, 1, WATCHDOG_MS);
      if (ret < 0 && errno == EINTR)
         continue;
      if (ret <= 0 || (pfd.revents & (POLLERR | POLLNVAL))) {
         fail("worker %s after %d ms without progress; killing it and its helper",
              ret == 0 ? "timed out" : "progress pipe failed", WATCHDOG_MS);
         kill_and_reap(child);
         close(progress[0]);
         return 1;
      }
      char bytes[256];
      while (read(progress[0], bytes, sizeof(bytes)) > 0)
         ;
      if (pfd.revents & POLLHUP) {
         int status;
         pid_t got;
         do {
            got = waitpid(child, &status, 0);
         } while (got < 0 && errno == EINTR);
         close(progress[0]);
         if (got != child) {
            fail("waitpid: %s", strerror(errno));
            return 1;
         }
         if (!WIFEXITED(status)) {
            fail("worker terminated by signal %d", WIFSIGNALED(status) ? WTERMSIG(status) : -1);
            return 1;
         }
         return WEXITSTATUS(status) == 0 ? 0 : 1;
      }
   }
}

int
main(int argc, char **argv)
{
   setvbuf(stdout, NULL, _IOLBF, 0);
   if (argc < 2 || argc > 3) {
      fprintf(stderr, "usage: %s <all|empty|command|multi|followup|queues> [iterations]\n",
              argv[0]);
      return 2;
   }
   const char *mode = argv[1];
   if (strcmp(mode, "all") && strcmp(mode, "empty") && strcmp(mode, "command") &&
       strcmp(mode, "multi") && strcmp(mode, "followup") && strcmp(mode, "queues")) {
      fprintf(stderr, "unknown mode: %s\n", mode);
      return 2;
   }
   unsigned iterations = 128;
   if (argc == 3) {
      char *end = NULL;
      errno = 0;
      unsigned long parsed = strtoul(argv[2], &end, 10);
      if (errno || !argv[2][0] || argv[2][0] == '-' || *end || !parsed || parsed > UINT_MAX) {
         fprintf(stderr, "invalid positive iteration count: %s\n", argv[2]);
         return 2;
      }
      iterations = (unsigned)parsed;
   }
   return supervise(mode, iterations);
}
