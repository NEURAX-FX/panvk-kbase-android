#define _GNU_SOURCE
#define VK_USE_PLATFORM_WAYLAND_KHR
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GL/gl.h>
#include <vulkan/vulkan.h>
#include <wayland-client.h>
#include <wayland-egl.h>
#include <wayland-server.h>
#include "linux-dmabuf-server-protocol.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <unistd.h>

/* Real Wayland SHM validation and real GPU rendering, on a private connection. */
struct results {
   _Atomic unsigned buffers, xr30_buffers, commits, bad;
   _Atomic unsigned default_feedback, surface_feedback;
   _Atomic uint32_t expected;
};
struct frame { struct wl_resource *resource; struct wl_list link; };
struct surface {
   struct results *results;
   struct wl_resource *pending;
   struct wl_list frames;
};
static void destroy(struct wl_client *c, struct wl_resource *r) { wl_resource_destroy(r); }
static void rect(struct wl_client *c, struct wl_resource *r, int32_t x, int32_t y, int32_t w, int32_t h) {}
static void region(struct wl_client *c, struct wl_resource *r, struct wl_resource *v) {}
static void integer(struct wl_client *c, struct wl_resource *r, int32_t v) {}
static const struct wl_region_interface region_impl = { destroy, rect, rect };
static void create_region(struct wl_client *c, struct wl_resource *r, uint32_t id)
{
   struct wl_resource *v = wl_resource_create(c, &wl_region_interface, 1, id);
   wl_resource_set_implementation(v, &region_impl, NULL, NULL);
}
static void attach(struct wl_client *c, struct wl_resource *r, struct wl_resource *b, int32_t x, int32_t y)
{
   struct surface *s = wl_resource_get_user_data(r);
   s->pending = b;
}
static void free_frame(struct wl_resource *r)
{
   struct frame *f = wl_resource_get_user_data(r);
   wl_list_remove(&f->link);
   free(f);
}
static void frame(struct wl_client *c, struct wl_resource *r, uint32_t id)
{
   struct surface *s = wl_resource_get_user_data(r);
   struct frame *f = calloc(1, sizeof(*f));
   assert(f);
   f->resource = wl_resource_create(c, &wl_callback_interface, 1, id);
   wl_resource_set_implementation(f->resource, NULL, f, free_frame);
   wl_list_insert(&s->frames, &f->link);
}
static void commit(struct wl_client *c, struct wl_resource *r)
{
   struct surface *s = wl_resource_get_user_data(r);
   if (s->pending) {
      struct wl_shm_buffer *b = wl_shm_buffer_get(s->pending);
      bool bad = !b;
      uint32_t pixel = 0, format = 0;
      if (b) {
         format = wl_shm_buffer_get_format(b);
         wl_shm_buffer_begin_access(b);
         memcpy(&pixel, wl_shm_buffer_get_data(b), sizeof(pixel));
         wl_shm_buffer_end_access(b);
         uint32_t mask = format == WL_SHM_FORMAT_XRGB2101010 ||
                         format == WL_SHM_FORMAT_ARGB2101010 ? 0x3fffffff : 0xffffff;
         bad = wl_shm_buffer_get_width(b) != 1 || wl_shm_buffer_get_height(b) != 1 ||
               (pixel & mask) != atomic_load(&s->results->expected);
      }
      printf("CONSUMER format=%08x pixel=%08x bad=%d\n", format, pixel, bad);
      atomic_fetch_add(&s->results->bad, bad);
      atomic_fetch_add(&s->results->commits, 1);
      wl_buffer_send_release(s->pending);
      s->pending = NULL;
   }
   struct frame *f, *tmp;
   wl_list_for_each_safe(f, tmp, &s->frames, link) {
      wl_callback_send_done(f->resource, atomic_load(&s->results->commits));
      wl_resource_destroy(f->resource);
   }
}
static void free_surface(struct wl_resource *r)
{
   struct surface *s = wl_resource_get_user_data(r);
   struct frame *f, *tmp;
   wl_list_for_each_safe(f, tmp, &s->frames, link) wl_resource_destroy(f->resource);
   free(s);
}
static const struct wl_surface_interface surface_impl = {
   .destroy = destroy, .attach = attach, .damage = rect, .frame = frame,
   .set_opaque_region = region, .set_input_region = region, .commit = commit,
   .set_buffer_transform = integer, .set_buffer_scale = integer, .damage_buffer = rect,
};
static void create_surface(struct wl_client *c, struct wl_resource *r, uint32_t id)
{
   struct surface *s = calloc(1, sizeof(*s));
   assert(s);
   s->results = wl_resource_get_user_data(r);
   wl_list_init(&s->frames);
   struct wl_resource *v = wl_resource_create(c, &wl_surface_interface, wl_resource_get_version(r), id);
   wl_resource_set_implementation(v, &surface_impl, s, free_surface);
}
static const struct wl_compositor_interface compositor_impl = {
   .create_surface = create_surface, .create_region = create_region,
};
static void bind_compositor(struct wl_client *c, void *data, uint32_t version, uint32_t id)
{
   struct wl_resource *r = wl_resource_create(c, &wl_compositor_interface, version, id);
   wl_resource_set_implementation(r, &compositor_impl, data, NULL);
}
static void unexpected_dmabuf(struct wl_client *c, struct wl_resource *r, uint32_t id)
{
   wl_resource_post_error(r, 0, "DMA-BUF import is not expected in the SHM transport test");
}
static const struct zwp_linux_dmabuf_feedback_v1_interface feedback_impl = { .destroy = destroy };
static void send_feedback(struct wl_client *c, uint32_t id)
{
   struct wl_resource *r = wl_resource_create(c, &zwp_linux_dmabuf_feedback_v1_interface, 4, id);
   wl_resource_set_implementation(r, &feedback_impl, NULL, NULL);
   const char *path = getenv("WAYLAND_FORMAT_DRM_DEVICE");
   struct stat st;
   assert(stat(path ? path : "/dev/dri/renderD128", &st) == 0 && S_ISCHR(st.st_mode));
   struct { uint32_t format, padding; uint64_t modifier; } table[] = {
      { 0x34325241, 0, 0 }, { 0x34325258, 0, 0 },
      { 0x30335241, 0, 0 }, { 0x30335258, 0, 0 },
   };
   int fd = memfd_create("format-feedback", MFD_CLOEXEC);
   assert(fd >= 0 && write(fd, table, sizeof(table)) == sizeof(table));
   struct wl_array dev = { .size = sizeof(st.st_rdev), .data = &st.st_rdev };
   uint16_t indices[] = { 0, 1, 2, 3 };
   struct wl_array formats = { .size = sizeof(indices), .data = indices };
   zwp_linux_dmabuf_feedback_v1_send_format_table(r, fd, sizeof(table));
   close(fd);
   zwp_linux_dmabuf_feedback_v1_send_main_device(r, &dev);
   zwp_linux_dmabuf_feedback_v1_send_tranche_target_device(r, &dev);
   zwp_linux_dmabuf_feedback_v1_send_tranche_flags(r, 0);
   zwp_linux_dmabuf_feedback_v1_send_tranche_formats(r, &formats);
   zwp_linux_dmabuf_feedback_v1_send_tranche_done(r);
   zwp_linux_dmabuf_feedback_v1_send_done(r);
}
static void default_feedback(struct wl_client *c, struct wl_resource *r, uint32_t id)
{
   struct results *results = wl_resource_get_user_data(r);
   atomic_fetch_add(&results->default_feedback, 1);
   send_feedback(c, id);
}
static void surface_feedback(struct wl_client *c, struct wl_resource *r, uint32_t id,
                             struct wl_resource *surface)
{
   struct results *results = wl_resource_get_user_data(r);
   atomic_fetch_add(&results->surface_feedback, 1);
   send_feedback(c, id);
}
static const struct zwp_linux_dmabuf_v1_interface dmabuf_impl = {
   .destroy = destroy, .create_params = unexpected_dmabuf,
   .get_default_feedback = default_feedback, .get_surface_feedback = surface_feedback,
};
static void bind_dmabuf(struct wl_client *c, void *data, uint32_t version, uint32_t id)
{
   struct wl_resource *r = wl_resource_create(c, &zwp_linux_dmabuf_v1_interface, version, id);
   wl_resource_set_implementation(r, &dmabuf_impl, data, NULL);
}
static void protocol(void *data, enum wl_protocol_logger_type type,
                     const struct wl_protocol_logger_message *msg)
{
   if (type != WL_PROTOCOL_LOGGER_REQUEST ||
       strcmp(wl_resource_get_class(msg->resource), "wl_shm_pool") ||
       strcmp(msg->message->name, "create_buffer")) return;
   struct results *r = data;
   uint32_t format = msg->arguments[5].u;
   atomic_fetch_add(&r->buffers, 1);
   atomic_fetch_add(&r->xr30_buffers, format == WL_SHM_FORMAT_XRGB2101010);
   printf("SHM_CREATE format=%08x\n", format);
}
static void run_server(int fd, struct results *r, bool ten, bool dmabuf)
{
   struct wl_display *d = wl_display_create();
   assert(d && wl_display_init_shm(d) == 0);
   if (ten) {
      wl_display_add_shm_format(d, WL_SHM_FORMAT_XRGB2101010);
      wl_display_add_shm_format(d, WL_SHM_FORMAT_ARGB2101010);
   }
   assert(wl_global_create(d, &wl_compositor_interface, 4, r, bind_compositor));
   if (dmabuf) assert(wl_global_create(d, &zwp_linux_dmabuf_v1_interface, 4, r, bind_dmabuf));
   assert(wl_display_add_protocol_logger(d, protocol, r));
   assert(wl_client_create(d, fd));
   wl_display_run(d);
   _exit(0);
}
static void global(void *data, struct wl_registry *r, uint32_t id, const char *name, uint32_t version)
{
   if (!strcmp(name, "wl_compositor"))
      *(struct wl_compositor **)data = wl_registry_bind(r, id, &wl_compositor_interface, version < 4 ? version : 4);
}
static void remove_global(void *data, struct wl_registry *r, uint32_t id) {}
static const struct wl_registry_listener listener = { global, remove_global };
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL line=%d: %s\n", __LINE__, #c); return false; } } while (0)

static bool test_egl(struct wl_display *d, struct wl_compositor *comp, struct results *r, bool ten, bool dmabuf)
{
   EGLDisplay e = eglGetPlatformDisplay(EGL_PLATFORM_WAYLAND_KHR, d, NULL);
   CHECK(e != EGL_NO_DISPLAY && eglInitialize(e, NULL, NULL));
   CHECK(eglBindAPI(EGL_OPENGL_API));
   const EGLint rgb8[] = { EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT, EGL_RED_SIZE, 8,
      EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_NONE };
   EGLConfig cfg;
   EGLint count, red, green, blue, alpha, types;
   CHECK(eglChooseConfig(e, rgb8, &cfg, 1, &count) && count == 1);
   CHECK(eglGetConfigAttrib(e, cfg, EGL_RED_SIZE, &red));
   CHECK(eglGetConfigAttrib(e, cfg, EGL_GREEN_SIZE, &green));
   CHECK(eglGetConfigAttrib(e, cfg, EGL_BLUE_SIZE, &blue));
   CHECK(eglGetConfigAttrib(e, cfg, EGL_ALPHA_SIZE, &alpha));
   CHECK(eglGetConfigAttrib(e, cfg, EGL_SURFACE_TYPE, &types));
   printf("EGL_SELECTED rgba=%d/%d/%d/%d surface_types=%x\n", red, green, blue, alpha, types);
   CHECK(red == (ten ? 10 : 8) && green == red && blue == red);
   const EGLint ctxattrs[] = { EGL_CONTEXT_MAJOR_VERSION, 4, EGL_CONTEXT_MINOR_VERSION, 6,
      EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT, EGL_NONE };
   EGLContext ctx = eglCreateContext(e, cfg, EGL_NO_CONTEXT, ctxattrs);
   CHECK(ctx != EGL_NO_CONTEXT);
   for (unsigned i = 0; i < 2; i++) {
      struct wl_surface *native = wl_compositor_create_surface(comp);
      struct wl_egl_window *window = wl_egl_window_create(native, 1, 1);
      CHECK(native && window);
      EGLSurface s = eglCreateWindowSurface(e, cfg, (EGLNativeWindowType)window, NULL);
      CHECK(s != EGL_NO_SURFACE && eglMakeCurrent(e, s, s, ctx));
      const char *renderer = (const char *)glGetString(GL_RENDERER);
      printf("GL_RENDERER=%s\n", renderer ? renderer : "NULL");
      CHECK(renderer && strstr(renderer, "MESA_PANVK"));
      CHECK(eglSwapInterval(e, 0));
      atomic_store(&r->expected, ten ? (i ? 0x000ffc00 : 0x3ff00000) : (i ? 0x0000ff00 : 0x00ff0000));
      glClearColor(!i, !!i, 0, 1);
      glClear(GL_COLOR_BUFFER_BIT);
      CHECK(glGetError() == GL_NO_ERROR && eglSwapBuffers(e, s));
      CHECK(wl_display_roundtrip(d) >= 0 && !wl_display_get_error(d));
      CHECK(atomic_load(&r->commits) == i + 1 && !atomic_load(&r->bad));
      CHECK(eglMakeCurrent(e, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT));
      CHECK(eglDestroySurface(e, s));
      wl_egl_window_destroy(window);
      wl_surface_destroy(native);
   }
   const EGLint rgb10window[] = { EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
      EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT, EGL_RED_SIZE, 10,
      EGL_GREEN_SIZE, 10, EGL_BLUE_SIZE, 10, EGL_NONE };
   CHECK(eglChooseConfig(e, rgb10window, &cfg, 1, &count));
   CHECK(count == (ten ? 1 : 0));
   const EGLint rgb10pbuffer[] = { EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
      EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT, EGL_RED_SIZE, 10,
      EGL_GREEN_SIZE, 10, EGL_BLUE_SIZE, 10, EGL_NONE };
   CHECK(eglChooseConfig(e, rgb10pbuffer, NULL, 0, &count) && count > 0);
   EGLConfig *configs = calloc(count, sizeof(*configs));
   CHECK(configs && eglChooseConfig(e, rgb10pbuffer, configs, count, &count));
   cfg = NULL;
   for (EGLint i = 0; i < count; i++) {
      CHECK(eglGetConfigAttrib(e, configs[i], EGL_RED_SIZE, &red));
      CHECK(eglGetConfigAttrib(e, configs[i], EGL_GREEN_SIZE, &green));
      CHECK(eglGetConfigAttrib(e, configs[i], EGL_BLUE_SIZE, &blue));
      if (red == 10 && green == 10 && blue == 10) { cfg = configs[i]; break; }
   }
   free(configs);
   CHECK(cfg && eglGetConfigAttrib(e, cfg, EGL_SURFACE_TYPE, &types));
   CHECK((types & EGL_PBUFFER_BIT) && (ten || !(types & EGL_WINDOW_BIT)));
   printf("EGL_PBUFFER rgb=10/10/10 surface_types=%x\n", types);
   const EGLint size[] = { EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE };
   EGLSurface p = eglCreatePbufferSurface(e, cfg, size);
   CHECK(p != EGL_NO_SURFACE && eglDestroySurface(e, p));
   if (dmabuf) CHECK(atomic_load(&r->default_feedback) > 0 && atomic_load(&r->surface_feedback) == 2);
   CHECK(eglDestroyContext(e, ctx) && eglTerminate(e));
   CHECK(wl_display_roundtrip(d) >= 0 && !wl_display_get_error(d));
   CHECK(ten ? atomic_load(&r->xr30_buffers) > 0 : atomic_load(&r->xr30_buffers) == 0);
   return true;
}

static bool test_vulkan(struct wl_display *d, struct wl_compositor *comp, struct results *r, bool ten)
{
   const char *exts[] = { VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WAYLAND_SURFACE_EXTENSION_NAME };
   VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_1 };
   VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
      .pApplicationInfo = &app, .enabledExtensionCount = 2, .ppEnabledExtensionNames = exts };
   VkInstance instance;
   CHECK(vkCreateInstance(&ici, NULL, &instance) == VK_SUCCESS);
   uint32_t count = 1;
   VkPhysicalDevice gpu;
   CHECK(vkEnumeratePhysicalDevices(instance, &count, &gpu) == VK_SUCCESS && count == 1);
   VkPhysicalDeviceProperties props;
   vkGetPhysicalDeviceProperties(gpu, &props);
   printf("VK_DEVICE=%s\n", props.deviceName);
   CHECK(strstr(props.deviceName, "Mali-G720"));
   struct wl_surface *native = wl_compositor_create_surface(comp);
   VkWaylandSurfaceCreateInfoKHR sci = { .sType = VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR,
      .display = d, .surface = native };
   VkSurfaceKHR surface;
   CHECK(vkCreateWaylandSurfaceKHR(instance, &sci, NULL, &surface) == VK_SUCCESS);
   count = 0;
   CHECK(vkGetPhysicalDeviceSurfaceFormatsKHR(gpu, surface, &count, NULL) == VK_SUCCESS);
   VkSurfaceFormatKHR *formats = calloc(count, sizeof(*formats));
   CHECK(formats && vkGetPhysicalDeviceSurfaceFormatsKHR(gpu, surface, &count, formats) == VK_SUCCESS);
   bool has10 = false, has8 = false;
   for (uint32_t i = 0; i < count; i++) {
      printf("VK_FORMAT format=%d colorspace=%d\n", formats[i].format, formats[i].colorSpace);
      if (formats[i].colorSpace != VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) continue;
      has10 |= formats[i].format == VK_FORMAT_A2R10G10B10_UNORM_PACK32;
      has8 |= formats[i].format == VK_FORMAT_B8G8R8A8_UNORM;
   }
   free(formats);
   CHECK(has8 && has10 == ten);
   uint32_t qcount = 0, q = UINT32_MAX;
   vkGetPhysicalDeviceQueueFamilyProperties(gpu, &qcount, NULL);
   VkQueueFamilyProperties *queues = calloc(qcount, sizeof(*queues));
   CHECK(queues);
   vkGetPhysicalDeviceQueueFamilyProperties(gpu, &qcount, queues);
   for (uint32_t i = 0; i < qcount; i++) {
      VkBool32 support = false;
      CHECK(vkGetPhysicalDeviceSurfaceSupportKHR(gpu, i, surface, &support) == VK_SUCCESS);
      if (support && (queues[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) { q = i; break; }
   }
   free(queues);
   CHECK(q != UINT32_MAX);
   float priority = 1;
   VkDeviceQueueCreateInfo qi = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
      .queueFamilyIndex = q, .queueCount = 1, .pQueuePriorities = &priority };
   const char *swap_ext = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
   VkDeviceCreateInfo di = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
      .queueCreateInfoCount = 1, .pQueueCreateInfos = &qi,
      .enabledExtensionCount = 1, .ppEnabledExtensionNames = &swap_ext };
   VkDevice device;
   CHECK(vkCreateDevice(gpu, &di, NULL, &device) == VK_SUCCESS);
   VkSurfaceCapabilitiesKHR caps;
   CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(gpu, surface, &caps) == VK_SUCCESS);
   VkSwapchainCreateInfoKHR ci = { .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
      .surface = surface, .minImageCount = caps.minImageCount,
      .imageFormat = VK_FORMAT_A2R10G10B10_UNORM_PACK32, .imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR,
      .imageExtent = {1, 1}, .imageArrayLayers = 1, .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
      .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE, .preTransform = caps.currentTransform,
      .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR, .presentMode = VK_PRESENT_MODE_FIFO_KHR,
      .clipped = VK_TRUE };
   VkSwapchainKHR chain = VK_NULL_HANDLE;
   unsigned before = atomic_load(&r->buffers);
   printf("VK_CREATE format=%d colorspace=%d mode=%d advertised=%d\n", ci.imageFormat, ci.imageColorSpace, ci.presentMode, ten);
   VkResult result = vkCreateSwapchainKHR(device, &ci, NULL, &chain);
   printf("VK_CREATE_RESULT=%d\n", result);
   CHECK(result == (ten ? VK_SUCCESS : VK_ERROR_INITIALIZATION_FAILED));
   CHECK(wl_display_roundtrip(d) >= 0 && !wl_display_get_error(d));
   if (ten) {
      CHECK(atomic_load(&r->xr30_buffers) > 0);
      vkDestroySwapchainKHR(device, chain, NULL);
   } else {
      CHECK(atomic_load(&r->buffers) == before);
   }
   ci.imageFormat = VK_FORMAT_B8G8R8A8_UNORM;
   CHECK(vkCreateSwapchainKHR(device, &ci, NULL, &chain) == VK_SUCCESS);
   CHECK(wl_display_roundtrip(d) >= 0 && !wl_display_get_error(d));
   CHECK(atomic_load(&r->buffers) > before);
   vkDestroySwapchainKHR(device, chain, NULL);
   vkDestroyDevice(device, NULL);
   vkDestroySurfaceKHR(instance, surface, NULL);
   vkDestroyInstance(instance, NULL);
   wl_surface_destroy(native);
   return true;
}

int main(int argc, char **argv)
{
   if (argc != 4 || (strcmp(argv[1], "egl") && strcmp(argv[1], "vulkan")) ||
       (strcmp(argv[2], "8") && strcmp(argv[2], "10")) ||
       (strcmp(argv[3], "shm") && strcmp(argv[3], "mixed"))) {
      fprintf(stderr, "usage: %s egl|vulkan 8|10 shm|mixed\n", argv[0]);
      return 2;
   }
   bool ten = !strcmp(argv[2], "10"), dmabuf = !strcmp(argv[3], "mixed");
   setvbuf(stdout, NULL, _IOLBF, 0);
   alarm(45);
   int fd[2];
   assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, fd) == 0);
   struct results *r = mmap(NULL, sizeof(*r), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
   assert(r != MAP_FAILED);
   pid_t child = fork();
   assert(child >= 0);
   if (!child) { close(fd[0]); run_server(fd[1], r, ten, dmabuf); }
   close(fd[1]);
   struct wl_display *d = wl_display_connect_to_fd(fd[0]);
   struct wl_compositor *comp = NULL;
   assert(d);
   struct wl_registry *reg = wl_display_get_registry(d);
   wl_registry_add_listener(reg, &listener, &comp);
   bool ok = wl_display_roundtrip(d) >= 0 && comp;
   if (ok) ok = !strcmp(argv[1], "egl") ? test_egl(d, comp, r, ten, dmabuf) : test_vulkan(d, comp, r, ten);
   printf("%s api=%s shm=%s dmabuf10=%d buffers=%u xr30=%u commits=%u bad=%u wl_error=%d\n",
      ok ? "PASS" : "FAIL", argv[1], argv[2], dmabuf, atomic_load(&r->buffers),
      atomic_load(&r->xr30_buffers), atomic_load(&r->commits), atomic_load(&r->bad), wl_display_get_error(d));
   wl_display_disconnect(d);
   kill(child, SIGTERM);
   waitpid(child, NULL, 0);
   munmap(r, sizeof(*r));
   return !ok;
}
