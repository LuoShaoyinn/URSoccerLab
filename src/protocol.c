#include "urs_protocol.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static uint16_t le16(const uint8_t *p) { return p[0] | (uint16_t)p[1] << 8; }
static uint32_t le32(const uint8_t *p) {
  return le16(p) | (uint32_t)le16(p + 2) << 16;
}
static uint32_t be32(const uint8_t *p) {
  return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 |
         p[3];
}
int urs_frame_header(uint8_t out[5], uint8_t type, size_t n) {
  if (n >= URS_MAX_FRAME)
    return -1;
  uint32_t v = (uint32_t)n + 1;
  out[0] = v >> 24;
  out[1] = v >> 16;
  out[2] = v >> 8;
  out[3] = v;
  out[4] = type;
  return 0;
}
void urs_parser_destroy(urs_parser *p) {
  free(p->buffer);
  memset(p, 0, sizeof(*p));
}
int urs_feed(urs_parser *p, const void *data, size_t n, urs_frame_fn cb,
             void *ctx) {
  const uint8_t *src = (const uint8_t *)data;
  while (n) {
    size_t target = 4;
    if (p->used >= 4) {
      uint32_t len = be32(p->buffer);
      if (!len || len > URS_MAX_FRAME)
        return -1;
      target = 4 + (size_t)len;
    }
    if (target > p->capacity) {
      void *b = realloc(p->buffer, target);
      if (!b)
        return -1;
      p->buffer = b;
      p->capacity = target;
    }
    size_t take = target - p->used;
    if (take > n)
      take = n;
    memcpy(p->buffer + p->used, src, take);
    p->used += take;
    src += take;
    n -= take;
    if (p->used == 4) {
      uint32_t len = be32(p->buffer);
      if (!len || len > URS_MAX_FRAME)
        return -1;
    }
    if (p->used == target && target > 4) {
      if (cb(ctx, p->buffer[4], p->buffer + 5, target - 5))
        return -1;
      p->used = 0;
    }
  }
  return 0;
}
int urs_parse_images(const uint8_t *p, size_t n, urs_image_fn cb, void *ctx) {
  if (n < 16 || p[0] != 2)
    return -1;
  uint32_t seq = le32(p + 4);
  uint64_t bits = (uint64_t)le32(p + 8) | (uint64_t)le32(p + 12) << 32;
  double time;
  memcpy(&time, &bits, 8);
  size_t off = 16;
  for (unsigned i = 0; i < p[1]; i++) {
    if (off >= n)
      return -1;
    unsigned names = p[off++];
    if (n - off < names + 15u)
      return -1;
    urs_image im = {0};
    memcpy(im.name, p + off, names);
    off += names;
    im.codec = p[off];
    im.pixel_format = p[off + 1];
    im.keyframe = p[off + 2] & 1;
    im.width = le16(p + off + 3);
    im.height = le16(p + off + 5);
    im.raw_size = le32(p + off + 7);
    im.size = le32(p + off + 11);
    off += 15;
    if (im.codec > 5 || im.pixel_format > 2 || !im.width || !im.height ||
        im.size > n - off)
      return -1;
    im.sequence = seq;
    im.sim_time = time;
    im.data = p + off;
    off += im.size;
    if (cb(ctx, &im))
      return -1;
  }
  return off == n ? 0 : -1;
}
int urs_parse_video(const urs_image *im, urs_video *v) {
  const uint8_t *p = im->data;
  size_t n = im->size;
  if (n < 19 || p[0] != 1 || p[1] > 1)
    return -1;
  memset(v, 0, sizeof(*v));
  v->layout = p[1];
  v->epoch = (uint64_t)le32(p + 2) | (uint64_t)le32(p + 6) << 32;
  v->width = le16(p + 10);
  v->height = le16(p + 12);
  v->config_size = le32(p + 14);
  size_t off = 19 + (size_t)p[18];
  if (off > n || v->config_size >= n - off || v->width < im->width ||
      v->height < im->height)
    return -1;
  memcpy(v->right_name, p + 19, p[18]);
  if (v->layout && (!p[18] || im->width % 2))
    return -1;
  v->config = p + off;
  v->packet = p + off + v->config_size;
  v->packet_size = n - off - v->config_size;
  return 0;
}
static int normalize(const double *q, double *r) {
  double norm = 0;
  for (int i = 0; i < 4; i++) {
    if (!isfinite(q[i]))
      return -1;
    norm = hypot(norm, q[i]);
  }
  if (norm < 1e-12 || !isfinite(norm))
    return -1;
  for (int i = 0; i < 4; i++)
    r[i] = q[i] / norm;
  return 0;
}
int urs_camera_json(char *out, size_t cap, const double p[3],
                    const double q[4]) {
  double r[4];
  if (normalize(q, r))
    return -1;
  for (int i = 0; i < 3; i++)
    if (!isfinite(p[i]) || fabs(p[i]) > 100)
      return -1;
  int n = snprintf(
      out, cap,
      "{\"version\":1,\"command\":\"set_camera\",\"args\":{\"translation_m\":[%"
      ".17g,%.17g,%.17g],\"rotation_quat_xyzw\":[%.17g,%.17g,%.17g,%.17g]}}",
      p[0], p[1], p[2], r[0], r[1], r[2], r[3]);
  return n >= 0 && (size_t)n < cap ? n : -1;
}
int urs_admin_json(char *out, size_t cap, const char *cmd, const char *actor,
                   const double *p, const double *q) {
  const char *allowed[] = {"get_pose", "set_pose", "reset", "lock_pose",
                           "unlock_pose"};
  int valid = 0;
  for (int i = 0; i < 5; i++)
    if (!strcmp(cmd, allowed[i]))
      valid = 1;
  if (!valid || !actor[0] || strlen(actor) > 255)
    return -1;
  char escaped[1531];
  size_t k = 0;
  for (const unsigned char *s = (const unsigned char *)actor; *s; s++) {
    if (*s < 32) {
      int z = snprintf(escaped + k, sizeof(escaped) - k, "\\u%04x", *s);
      k += (size_t)z;
    } else {
      if (*s == '"' || *s == '\\')
        escaped[k++] = '\\';
      escaped[k++] = (char)*s;
    }
  }
  escaped[k] = 0;
  int n =
      snprintf(out, cap, "{\"command\":\"%s\",\"args\":{\"actor_id\":\"%s\"",
               cmd, escaped);
  if (n < 0 || (size_t)n >= cap)
    return -1;
  if (p) {
    for (int i = 0; i < 3; i++)
      if (!isfinite(p[i]))
        return -1;
    int z = snprintf(out + n, cap - n, ",\"translation_m\":[%.17g,%.17g,%.17g]",
                     p[0], p[1], p[2]);
    if (z < 0 || (size_t)z >= cap - (size_t)n)
      return -1;
    n += z;
  }
  if (q) {
    double r[4];
    if (normalize(q, r))
      return -1;
    int z = snprintf(out + n, cap - n,
                     ",\"rotation_quat_xyzw\":[%.17g,%.17g,%.17g,%.17g]", r[0],
                     r[1], r[2], r[3]);
    if (z < 0 || (size_t)z >= cap - (size_t)n)
      return -1;
    n += z;
  }
  if (cap - (size_t)n < 3)
    return -1;
  out[n++] = '}';
  out[n++] = '}';
  out[n] = 0;
  return n;
}
