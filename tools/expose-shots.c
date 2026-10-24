/* expose-shots: one still picture of every window, whatever desktop it is on.
 *
 *   expose-shots DIR [MAX_WIDTH]
 *
 * Binds ext-foreign-toplevel-list (the windows) and ext-image-copy-capture with ext-image-capture-source (a picture of one
 * window), takes one frame of each, scales it down to MAX_WIDTH (default 640) and writes DIR/<identifier>.raw:
 *   a line "WIDTH HEIGHT STRIDE\n" and then WIDTH*HEIGHT BGRA pixels (premultiplied, little endian, 4 bytes each).
 * <identifier> is the window's ext-foreign-toplevel identifier; Umbriel uses the same string as the window "id".
 * Prints "ok <identifier> <width> <height>" or "fail <identifier>" for every window, then "done".
 */
#define _GNU_SOURCE
#include <errno.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>
#include "ext-foreign-toplevel-list-v1-client-protocol.h"
#include "ext-image-capture-source-v1-client-protocol.h"
#include "ext-image-copy-capture-v1-client-protocol.h"

static struct wl_display *display;
static struct wl_shm *shm;
static struct ext_foreign_toplevel_list_v1 *list;
static struct ext_foreign_toplevel_image_capture_source_manager_v1 *source_mgr;
static struct ext_image_copy_capture_manager_v1 *copy_mgr;

struct toplevel { struct ext_foreign_toplevel_handle_v1 *handle; char id[64]; int closed; };
static struct toplevel tops[256];
static int ntops;

static void tl_closed(void *d, struct ext_foreign_toplevel_handle_v1 *h) { ((struct toplevel *)d)->closed = 1; }
static void tl_done(void *d, struct ext_foreign_toplevel_handle_v1 *h) {}
static void tl_title(void *d, struct ext_foreign_toplevel_handle_v1 *h, const char *t) {}
static void tl_app(void *d, struct ext_foreign_toplevel_handle_v1 *h, const char *a) {}
static void tl_id(void *d, struct ext_foreign_toplevel_handle_v1 *h, const char *id) {
  snprintf(((struct toplevel *)d)->id, sizeof(((struct toplevel *)d)->id), "%s", id);
}
static const struct ext_foreign_toplevel_handle_v1_listener tl_listener = { tl_closed, tl_done, tl_title, tl_app, tl_id };

static void list_toplevel(void *d, struct ext_foreign_toplevel_list_v1 *l, struct ext_foreign_toplevel_handle_v1 *h) {
  if (ntops >= 256) return;
  struct toplevel *t = &tops[ntops++];
  memset(t, 0, sizeof *t);
  t->handle = h;
  ext_foreign_toplevel_handle_v1_add_listener(h, &tl_listener, t);
}
static void list_finished(void *d, struct ext_foreign_toplevel_list_v1 *l) {}
static const struct ext_foreign_toplevel_list_v1_listener list_listener = { list_toplevel, list_finished };

static void reg_global(void *d, struct wl_registry *r, uint32_t name, const char *iface, uint32_t ver) {
  if (!strcmp(iface, wl_shm_interface.name)) shm = wl_registry_bind(r, name, &wl_shm_interface, 1);
  else if (!strcmp(iface, ext_foreign_toplevel_list_v1_interface.name)) list = wl_registry_bind(r, name, &ext_foreign_toplevel_list_v1_interface, 1);
  else if (!strcmp(iface, ext_foreign_toplevel_image_capture_source_manager_v1_interface.name))
    source_mgr = wl_registry_bind(r, name, &ext_foreign_toplevel_image_capture_source_manager_v1_interface, 1);
  else if (!strcmp(iface, ext_image_copy_capture_manager_v1_interface.name))
    copy_mgr = wl_registry_bind(r, name, &ext_image_copy_capture_manager_v1_interface, 1);
}
static void reg_remove(void *d, struct wl_registry *r, uint32_t name) {}
static const struct wl_registry_listener reg_listener = { reg_global, reg_remove };

/* one capture */
struct cap { int bw, bh; uint32_t format; int have_format; int done; int ok; };
static void s_size(void *d, struct ext_image_copy_capture_session_v1 *s, uint32_t w, uint32_t h) { ((struct cap *)d)->bw = w; ((struct cap *)d)->bh = h; }
static void s_shm(void *d, struct ext_image_copy_capture_session_v1 *s, uint32_t f) {
  struct cap *c = d;
  if (!c->have_format || f == WL_SHM_FORMAT_ARGB8888) { c->format = f; c->have_format = 1; }
}
static void s_dev(void *d, struct ext_image_copy_capture_session_v1 *s, struct wl_array *a) {}
static void s_fmt(void *d, struct ext_image_copy_capture_session_v1 *s, uint32_t f, struct wl_array *m) {}
static void s_done(void *d, struct ext_image_copy_capture_session_v1 *s) { ((struct cap *)d)->done = 1; }
static void s_stopped(void *d, struct ext_image_copy_capture_session_v1 *s) { ((struct cap *)d)->done = 2; }
static const struct ext_image_copy_capture_session_v1_listener s_listener = { s_size, s_shm, s_dev, s_fmt, s_done, s_stopped };

static void f_transform(void *d, struct ext_image_copy_capture_frame_v1 *f, uint32_t t) {}
static void f_damage(void *d, struct ext_image_copy_capture_frame_v1 *f, int32_t x, int32_t y, int32_t w, int32_t h) {}
static void f_time(void *d, struct ext_image_copy_capture_frame_v1 *f, uint32_t a, uint32_t b, uint32_t c) {}
static void f_ready(void *d, struct ext_image_copy_capture_frame_v1 *f) { ((struct cap *)d)->done = 3; ((struct cap *)d)->ok = 1; }
static void f_failed(void *d, struct ext_image_copy_capture_frame_v1 *f, uint32_t r) { ((struct cap *)d)->done = 3; ((struct cap *)d)->ok = 0; }
static const struct ext_image_copy_capture_frame_v1_listener f_listener = { f_transform, f_damage, f_time, f_ready, f_failed };

static int wait_for(struct cap *c, int target_min, int ms) {
  struct timespec t0; clock_gettime(CLOCK_MONOTONIC, &t0);
  while (c->done < target_min) {
    wl_display_flush(display);
    struct pollfd p = { wl_display_get_fd(display), POLLIN, 0 };
    while (wl_display_prepare_read(display) != 0) wl_display_dispatch_pending(display);
    int r = poll(&p, 1, 50);
    if (r > 0) wl_display_read_events(display); else wl_display_cancel_read(display);
    wl_display_dispatch_pending(display);
    struct timespec t1; clock_gettime(CLOCK_MONOTONIC, &t1);
    if ((t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000 > ms) return 0;
    if (c->done == 2) return 0;
  }
  return 1;
}

static int shoot(struct toplevel *t, const char *dir, int maxw) {
  struct ext_image_capture_source_v1 *src = ext_foreign_toplevel_image_capture_source_manager_v1_create_source(source_mgr, t->handle);
  struct ext_image_copy_capture_session_v1 *sess = ext_image_copy_capture_manager_v1_create_session(copy_mgr, src, 0);
  struct cap c; memset(&c, 0, sizeof c);
  ext_image_copy_capture_session_v1_add_listener(sess, &s_listener, &c);
  int ok = 0;
  if (!wait_for(&c, 1, 800) || c.done != 1 || !c.have_format || c.bw <= 0 || c.bh <= 0) goto out;
  size_t stride = (size_t)c.bw * 4, size = stride * c.bh;
  int fd = memfd_create("shot", 0);
  if (fd < 0 || ftruncate(fd, size) < 0) goto out;
  uint8_t *map = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, size);
  struct wl_buffer *buf = wl_shm_pool_create_buffer(pool, 0, c.bw, c.bh, stride, c.format);
  struct ext_image_copy_capture_frame_v1 *frame = ext_image_copy_capture_session_v1_create_frame(sess);
  c.done = 0;
  ext_image_copy_capture_frame_v1_add_listener(frame, &f_listener, &c);
  ext_image_copy_capture_frame_v1_attach_buffer(frame, buf);
  ext_image_copy_capture_frame_v1_damage_buffer(frame, 0, 0, c.bw, c.bh);
  ext_image_copy_capture_frame_v1_capture(frame);
  if (wait_for(&c, 3, 1500) && c.ok) {
    int ow = c.bw > maxw ? maxw : c.bw;
    int oh = (int)((long)c.bh * ow / c.bw); if (oh < 1) oh = 1;
    char path[512], tmp[540];
    snprintf(path, sizeof path, "%s/%s.raw", dir, t->id); snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    if (f) {
      fprintf(f, "%d %d %d\n", ow, oh, ow * 4);
      uint8_t *row = malloc((size_t)ow * 4);
      int opaque = c.format == WL_SHM_FORMAT_XRGB8888;
      for (int y = 0; y < oh; y++) {                           /* box filter: average the source block of each output pixel */
        int sy0 = (long)y * c.bh / oh, sy1 = (long)(y + 1) * c.bh / oh; if (sy1 <= sy0) sy1 = sy0 + 1;
        for (int x = 0; x < ow; x++) {
          int sx0 = (long)x * c.bw / ow, sx1 = (long)(x + 1) * c.bw / ow; if (sx1 <= sx0) sx1 = sx0 + 1;
          unsigned acc[4] = {0, 0, 0, 0}; int n = 0;
          for (int sy = sy0; sy < sy1; sy++) for (int sx = sx0; sx < sx1; sx++) {
            uint8_t *p = map + sy * stride + (size_t)sx * 4;
            acc[0] += p[0]; acc[1] += p[1]; acc[2] += p[2]; acc[3] += opaque ? 255 : p[3]; n++;
          }
          for (int k = 0; k < 4; k++) row[x * 4 + k] = acc[k] / n;
        }
        fwrite(row, 1, (size_t)ow * 4, f);
      }
      free(row); fclose(f); rename(tmp, path);
      printf("ok %s %d %d\n", t->id, ow, oh); ok = 1;
    }
  }
  ext_image_copy_capture_frame_v1_destroy(frame);
  wl_buffer_destroy(buf); wl_shm_pool_destroy(pool); munmap(map, size); close(fd);
out:
  ext_image_copy_capture_session_v1_destroy(sess);
  ext_image_capture_source_v1_destroy(src);
  if (!ok) printf("fail %s\n", t->id);
  return ok;
}

int main(int argc, char **argv) {
  if (argc < 2) { fprintf(stderr, "usage: expose-shots DIR [MAX_WIDTH]\n"); return 2; }
  int maxw = argc > 2 ? atoi(argv[2]) : 640;
  display = wl_display_connect(NULL);
  if (!display) { fprintf(stderr, "expose-shots: no Wayland display\n"); return 1; }
  struct wl_registry *reg = wl_display_get_registry(display);
  wl_registry_add_listener(reg, &reg_listener, NULL);
  wl_display_roundtrip(display);
  if (!shm || !list || !source_mgr || !copy_mgr) { fprintf(stderr, "expose-shots: the compositor cannot capture single windows\n"); return 1; }
  ext_foreign_toplevel_list_v1_add_listener(list, &list_listener, NULL);
  wl_display_roundtrip(display);
  wl_display_roundtrip(display);               /* the identifiers arrive after the handles */
  for (int i = 0; i < ntops; i++) if (tops[i].id[0] && !tops[i].closed) shoot(&tops[i], argv[1], maxw);
  printf("done\n");
  return 0;
}
