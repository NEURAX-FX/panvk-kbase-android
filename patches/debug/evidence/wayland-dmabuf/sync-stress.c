/* Sync stress client: fullscreen EGL/GLES (Zink+PanVK) Wayland window. Each
 * frame runs an expensive full-screen fragment pass whose output is one
 * solid palette color (cycling A,B,C). A compositor that samples the buffer
 * before the GPU finished sees the previous color in some tiles, or
 * unwritten/garbage pixels. Throwaway. */
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <wayland-client.h>
#include <wayland-egl.h>
#include "xdg-shell-client-protocol.h"

static struct wl_compositor *comp;
static struct xdg_wm_base *wm;
static int width = 0, height = 0, configured = 0, closed = 0;

static void ping(void *d, struct xdg_wm_base *b, uint32_t s) { (void)d; xdg_wm_base_pong(b, s); }
static const struct xdg_wm_base_listener wm_l = { ping };
static void reg(void *d, struct wl_registry *r, uint32_t id, const char *i, uint32_t v)
{
   (void)d; (void)v;
   if (!strcmp(i, "wl_compositor")) comp = wl_registry_bind(r, id, &wl_compositor_interface, 4);
   else if (!strcmp(i, "xdg_wm_base")) { wm = wl_registry_bind(r, id, &xdg_wm_base_interface, 1); xdg_wm_base_add_listener(wm, &wm_l, NULL); }
}
static void reg_rm(void *d, struct wl_registry *r, uint32_t id) { (void)d; (void)r; (void)id; }
static const struct wl_registry_listener reg_l = { reg, reg_rm };
static void surf_conf(void *d, struct xdg_surface *s, uint32_t serial) { (void)d; xdg_surface_ack_configure(s, serial); configured = 1; }
static const struct xdg_surface_listener surf_l = { surf_conf };
static void top_conf(void *d, struct xdg_toplevel *t, int32_t w, int32_t h, struct wl_array *st)
{ (void)d; (void)t; (void)st; if (w > 0 && h > 0) { width = w; height = h; } }
static void top_close(void *d, struct xdg_toplevel *t) { (void)d; (void)t; closed = 1; }
static const struct xdg_toplevel_listener top_l = { top_conf, top_close };

static const char *vs = "attribute vec2 p; void main(){ gl_Position = vec4(p,0.0,1.0); }";
static const char *fs =
   "precision highp float; uniform vec3 c; uniform int n;\n"
   "void main(){ float a = gl_FragCoord.x * 0.001 + gl_FragCoord.y * 0.002;\n"
   "  for (int i = 0; i < 4096; i++) { if (i >= n) break; a = fract(sin(a) * 43758.5453 + 0.1); }\n"
   "  gl_FragColor = vec4(c + vec3(a * 1e-9), 1.0); }";

static GLuint sh(GLenum t, const char *s)
{
   GLuint o = glCreateShader(t); glShaderSource(o, 1, &s, NULL); glCompileShader(o);
   GLint ok; glGetShaderiv(o, GL_COMPILE_STATUS, &ok);
   if (!ok) { char l[512]; glGetShaderInfoLog(o, 512, NULL, l); fprintf(stderr, "shader: %s\n", l); exit(2); }
   return o;
}

int main(int argc, char **argv)
{
   int frames = argc > 1 ? atoi(argv[1]) : 600, iters = argc > 2 ? atoi(argv[2]) : 200;
   struct wl_display *dpy = wl_display_connect(NULL);
   if (!dpy) { fprintf(stderr, "no wayland\n"); return 2; }
   wl_registry_add_listener(wl_display_get_registry(dpy), &reg_l, NULL);
   wl_display_roundtrip(dpy);
   struct wl_surface *ws = wl_compositor_create_surface(comp);
   struct xdg_surface *xs = xdg_wm_base_get_xdg_surface(wm, ws);
   xdg_surface_add_listener(xs, &surf_l, NULL);
   struct xdg_toplevel *top = xdg_surface_get_toplevel(xs);
   xdg_toplevel_add_listener(top, &top_l, NULL);
   xdg_toplevel_set_app_id(top, "sync-stress");
   xdg_toplevel_set_fullscreen(top, NULL);
   wl_surface_commit(ws);
   while (!configured) wl_display_dispatch(dpy);
   if (!width) { width = 2392; height = 1080; }

   EGLDisplay ed = eglGetDisplay((EGLNativeDisplayType)dpy);
   eglInitialize(ed, NULL, NULL);
   eglBindAPI(EGL_OPENGL_ES_API);
   const EGLint ca[] = { EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
                         EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 0, EGL_NONE };
   EGLConfig cfg; EGLint n;
   if (!eglChooseConfig(ed, ca, &cfg, 1, &n) || !n) { fprintf(stderr, "config\n"); return 2; }
   const EGLint xa[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
   EGLContext ctx = eglCreateContext(ed, cfg, EGL_NO_CONTEXT, xa);
   struct wl_egl_window *ew = wl_egl_window_create(ws, width, height);
   EGLSurface es = eglCreateWindowSurface(ed, cfg, (EGLNativeWindowType)ew, NULL);
   eglMakeCurrent(ed, es, es, ctx);
   eglSwapInterval(ed, 0);
   printf("STRESS renderer=%s size=%dx%d frames=%d iters=%d\n", glGetString(GL_RENDERER), width, height, frames, iters);

   GLuint prog = glCreateProgram();
   glAttachShader(prog, sh(GL_VERTEX_SHADER, vs)); glAttachShader(prog, sh(GL_FRAGMENT_SHADER, fs));
   glBindAttribLocation(prog, 0, "p"); glLinkProgram(prog); glUseProgram(prog);
   const GLfloat tri[] = { -1, -1, 3, -1, -1, 3 };
   glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, tri); glEnableVertexAttribArray(0);
   GLint uc = glGetUniformLocation(prog, "c"), un = glGetUniformLocation(prog, "n");
   glUniform1i(un, iters);
   glViewport(0, 0, width, height);
   /* Palette in RGB bytes: red, green, blue-ish; all distinct per channel. */
   const float pal[3][3] = { { 0xd0 / 255.f, 0x30 / 255.f, 0x30 / 255.f },
                             { 0x30 / 255.f, 0xd0 / 255.f, 0x30 / 255.f },
                             { 0x30 / 255.f, 0x30 / 255.f, 0xd0 / 255.f } };
   struct timespec t0, t1; clock_gettime(CLOCK_MONOTONIC, &t0);
   for (int f = 0; f < frames && !closed; f++) {
      glUniform3fv(uc, 1, pal[f % 3]);
      glDrawArrays(GL_TRIANGLES, 0, 3);
      eglSwapBuffers(ed, es);
      wl_display_dispatch_pending(dpy);
   }
   clock_gettime(CLOCK_MONOTONIC, &t1);
   double s = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
   printf("STRESS_DONE frames=%d seconds=%.2f fps=%.1f\n", frames, s, frames / s);
   return 0;
}
