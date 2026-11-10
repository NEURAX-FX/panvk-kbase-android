#define _GNU_SOURCE
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <wayland-client.h>
#include <wayland-egl.h>
#include <wayland-server.h>
#include <errno.h>
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
#include <unistd.h>

#define SIZE 64
struct results {
   _Atomic unsigned commits;
   _Atomic unsigned bad;
   _Atomic uint32_t expected;
};
struct test_surface {
   struct results *results;
   struct wl_resource *pending;
   struct wl_list callbacks;
};
struct frame {
   struct wl_resource *resource;
   struct wl_list link;
};

static void resource_destroy(struct wl_client *client, struct wl_resource *res)
{
   (void)client;
   wl_resource_destroy(res);
}
static void region_rect(struct wl_client *client, struct wl_resource *res,
                        int32_t x, int32_t y, int32_t w, int32_t h)
{
   (void)client; (void)res; (void)x; (void)y; (void)w; (void)h;
}
static const struct wl_region_interface region_impl = {
   .destroy = resource_destroy, .add = region_rect, .subtract = region_rect,
};
static void create_region(struct wl_client *client, struct wl_resource *compositor,
                          uint32_t id)
{
   (void)compositor;
   struct wl_resource *res = wl_resource_create(client, &wl_region_interface, 1, id);
   wl_resource_set_implementation(res, &region_impl, NULL, NULL);
}
static void attach(struct wl_client *client, struct wl_resource *res,
                   struct wl_resource *buffer, int32_t x, int32_t y)
{
   (void)client; (void)x; (void)y;
   struct test_surface *s = wl_resource_get_user_data(res);
   s->pending = buffer;
}
static void frame_destroy(struct wl_resource *res)
{
   struct frame *f = wl_resource_get_user_data(res);
   wl_list_remove(&f->link);
   free(f);
}
static void request_frame(struct wl_client *client, struct wl_resource *res, uint32_t id)
{
   struct test_surface *s = wl_resource_get_user_data(res);
   struct frame *f = calloc(1, sizeof(*f));
   f->resource = wl_resource_create(client, &wl_callback_interface, 1, id);
   wl_resource_set_implementation(f->resource, NULL, f, frame_destroy);
   wl_list_insert(&s->callbacks, &f->link);
}
static void set_region(struct wl_client *client, struct wl_resource *res,
                       struct wl_resource *region)
{
   (void)client; (void)res; (void)region;
}
static void set_int(struct wl_client *client, struct wl_resource *res, int32_t n)
{
   (void)client; (void)res; (void)n;
}
static void commit(struct wl_client *client, struct wl_resource *res)
{
   (void)client;
   struct test_surface *s = wl_resource_get_user_data(res);
   if (s->pending) {
      struct wl_shm_buffer *buffer = wl_shm_buffer_get(s->pending);
      uint32_t expected = atomic_load(&s->results->expected);
      bool bad = !buffer;
      uint32_t first = 0;
      if (buffer) {
         int width = wl_shm_buffer_get_width(buffer);
         int height = wl_shm_buffer_get_height(buffer);
         int stride = wl_shm_buffer_get_stride(buffer);
         bad = width != SIZE || height != SIZE;
         wl_shm_buffer_begin_access(buffer);
         const uint8_t *data = wl_shm_buffer_get_data(buffer);
         memcpy(&first, data, sizeof(first));
         for (int y = 0; y < height && !bad; y++) {
            const uint32_t *row = (const uint32_t *)(data + y * stride);
            for (int x = 0; x < width; x++)
               if (row[x] != expected) { bad = true; break; }
         }
         wl_shm_buffer_end_access(buffer);
      }
      if (bad) {
         fprintf(stderr, "CONSUMER_BAD surface=%u expected=%08x first=%08x\n",
                 wl_resource_get_id(res), expected, first);
         atomic_fetch_add(&s->results->bad, 1);
      }
      atomic_fetch_add(&s->results->commits, 1);
      wl_buffer_send_release(s->pending);
      s->pending = NULL;
   }
   struct frame *f, *tmp;
   wl_list_for_each_safe(f, tmp, &s->callbacks, link) {
      wl_callback_send_done(f->resource, atomic_load(&s->results->commits));
      wl_resource_destroy(f->resource);
   }
}
static void surface_destroy(struct wl_resource *res)
{
   struct test_surface *s = wl_resource_get_user_data(res);
   struct frame *f, *tmp;
   wl_list_for_each_safe(f, tmp, &s->callbacks, link)
      wl_resource_destroy(f->resource);
   free(s);
}
static const struct wl_surface_interface surface_impl = {
   .destroy = resource_destroy, .attach = attach, .damage = region_rect,
   .frame = request_frame, .set_opaque_region = set_region,
   .set_input_region = set_region, .commit = commit,
   .set_buffer_transform = set_int, .set_buffer_scale = set_int,
   .damage_buffer = region_rect,
};
static void create_surface(struct wl_client *client, struct wl_resource *compositor,
                           uint32_t id)
{
   struct test_surface *s = calloc(1, sizeof(*s));
   s->results = wl_resource_get_user_data(compositor);
   wl_list_init(&s->callbacks);
   struct wl_resource *res = wl_resource_create(client, &wl_surface_interface,
                                               wl_resource_get_version(compositor), id);
   wl_resource_set_implementation(res, &surface_impl, s, surface_destroy);
}
static const struct wl_compositor_interface compositor_impl = {
   .create_surface = create_surface, .create_region = create_region,
};
static void bind_compositor(struct wl_client *client, void *data,
                            uint32_t version, uint32_t id)
{
   struct wl_resource *res = wl_resource_create(client, &wl_compositor_interface,
                                               version, id);
   wl_resource_set_implementation(res, &compositor_impl, data, NULL);
}
struct client_end { struct wl_listener listener; struct wl_display *display; };
static void client_gone(struct wl_listener *listener, void *data)
{
   (void)data;
   struct client_end *end = wl_container_of(listener, end, listener);
   wl_display_terminate(end->display);
}
static void run_server(int fd, struct results *results)
{
   struct wl_display *display = wl_display_create();
   if (!display || wl_display_init_shm(display) < 0) _exit(2);
   if (!wl_global_create(display, &wl_compositor_interface, 4, results, bind_compositor))
      _exit(2);
   struct wl_client *client = wl_client_create(display, fd);
   if (!client) _exit(2);
   struct client_end end = { .listener.notify = client_gone, .display = display };
   wl_client_add_destroy_listener(client, &end.listener);
   wl_display_run(display);
   wl_display_destroy_clients(display);
   wl_display_destroy(display);
   _exit(0);
}
static void global(void *data, struct wl_registry *registry, uint32_t name,
                   const char *interface, uint32_t version)
{
   if (!strcmp(interface, "wl_compositor"))
      *(struct wl_compositor **)data = wl_registry_bind(registry, name,
         &wl_compositor_interface, version < 4 ? version : 4);
}
static void global_remove(void *data, struct wl_registry *registry, uint32_t name)
{
   (void)data; (void)registry; (void)name;
}
static const struct wl_registry_listener registry_listener = {
   .global = global, .global_remove = global_remove,
};

int main(int argc, char **argv)
{
   const char *mode = argc > 1 ? argv[1] : "--recreate";
   bool rebind = !strcmp(mode, "--rebind");
   bool make_current = !strcmp(mode, "--make-current");
   bool destroy_current = !strcmp(mode, "--destroy-current");
   if (argc > 2 || (!rebind && !make_current && !destroy_current &&
                    strcmp(mode, "--recreate"))) {
      fprintf(stderr, "usage: %s [--recreate|--rebind|--make-current|--destroy-current]\n", argv[0]);
      return 2;
   }
   setvbuf(stdout, NULL, _IOLBF, 0);
   alarm(60);
   int fds[2];
   if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, fds)) return 2;
   struct results *results = mmap(NULL, sizeof(*results), PROT_READ | PROT_WRITE,
                                  MAP_SHARED | MAP_ANONYMOUS, -1, 0);
   if (results == MAP_FAILED) return 2;
   pid_t child = fork();
   if (child < 0) return 2;
   if (!child) { close(fds[0]); run_server(fds[1], results); }
   close(fds[1]);
   int status = 1;
   struct wl_display *display = wl_display_connect_to_fd(fds[0]);
   struct wl_compositor *compositor = NULL;
   EGLDisplay egl = EGL_NO_DISPLAY;
   EGLContext ctx = EGL_NO_CONTEXT;
   EGLSurface es = EGL_NO_SURFACE;
   struct wl_surface *native = NULL;
   struct wl_egl_window *window = NULL;
   if (!display) goto finish;
   struct wl_registry *registry = wl_display_get_registry(display);
   wl_registry_add_listener(registry, &registry_listener, &compositor);
   if (wl_display_roundtrip(display) < 0 || !compositor) goto finish;
   egl = eglGetPlatformDisplay(EGL_PLATFORM_WAYLAND_KHR, display, NULL);
   if (egl == EGL_NO_DISPLAY || !eglInitialize(egl, NULL, NULL)) goto finish;
   eglBindAPI(EGL_OPENGL_ES_API);
   const EGLint attribs[] = {
      EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
      EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE,
   };
   EGLConfig config;
   EGLint count;
   if (!eglChooseConfig(egl, attribs, &config, 1, &count) || count != 1) goto finish;
   const EGLint context_attribs[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
   ctx = eglCreateContext(egl, config, EGL_NO_CONTEXT, context_attribs);
   if (ctx == EGL_NO_CONTEXT) goto finish;
   uintptr_t previous = 0;
   unsigned reuses = 0;
   for (unsigned i = 0; i < 128; i++) {
      if (!native) native = wl_compositor_create_surface(compositor);
      if ((uintptr_t)native == previous) reuses++;
      printf("CLIENT iteration=%u native=%p id=%u reuse=%u mode=%s\n", i,
             (void *)native, wl_proxy_get_id((struct wl_proxy *)native), reuses,
             mode + 2);
      if (!window) window = wl_egl_window_create(native, SIZE, SIZE);
      if (!window) goto finish;
      if (es == EGL_NO_SURFACE)
         es = eglCreateWindowSurface(egl, config, (EGLNativeWindowType)window, NULL);
      if (es == EGL_NO_SURFACE || !eglMakeCurrent(egl, es, es, ctx)) goto finish;
      if (!i) {
         const char *renderer = (const char *)glGetString(GL_RENDERER);
         printf("GL_RENDERER=%s\n", renderer ? renderer : "<null>");
         if (!renderer || !strstr(renderer, "MESA_PANVK")) goto finish;
      }
      eglSwapInterval(egl, 0);
      uint32_t color = i % 3 == 0 ? 0xffff0000 : i % 3 == 1 ? 0xff00ff00 : 0xff0000ff;
      atomic_store(&results->expected, color);
      glClearColor(i % 3 == 0, i % 3 == 1, i % 3 == 2, 1.0f);
      glClear(GL_COLOR_BUFFER_BIT);
      if (glGetError() != GL_NO_ERROR || !eglSwapBuffers(egl, es)) goto finish;
      if (wl_display_roundtrip(display) < 0) goto finish;
      if (atomic_load(&results->bad) || atomic_load(&results->commits) != i + 1)
         goto finish;
      if (destroy_current) {
         if (!eglDestroySurface(egl, es)) goto finish;
         es = EGL_NO_SURFACE;
      }
      if (!eglMakeCurrent(egl, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT))
         goto finish;
      previous = (uintptr_t)native;
      if (!make_current) {
         if (!destroy_current && !eglDestroySurface(egl, es)) goto finish;
         es = EGL_NO_SURFACE;
         wl_egl_window_destroy(window);
         window = NULL;
         if (!rebind) { wl_surface_destroy(native); native = NULL; }
      }
   }
   printf("PASS mode=%s commits=%u bad=%u pointer_reuses=%u\n",
          mode + 2, atomic_load(&results->commits),
          atomic_load(&results->bad), reuses);
   status = 0;
finish:
   if (status)
      fprintf(stderr, "FAIL EGL=%04x Wayland=%d commits=%u bad=%u\n",
              eglGetError(), display ? wl_display_get_error(display) : errno,
              atomic_load(&results->commits), atomic_load(&results->bad));
   if (egl != EGL_NO_DISPLAY) {
      eglMakeCurrent(egl, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
      if (es != EGL_NO_SURFACE) eglDestroySurface(egl, es);
      if (ctx != EGL_NO_CONTEXT) eglDestroyContext(egl, ctx);
      eglTerminate(egl);
   }
   if (window) wl_egl_window_destroy(window);
   if (native) wl_surface_destroy(native);
   if (display) wl_display_disconnect(display);
   kill(child, SIGTERM);
   waitpid(child, NULL, 0);
   munmap(results, sizeof(*results));
   return status;
}
