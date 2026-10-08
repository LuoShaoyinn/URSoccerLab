#include "urs_protocol.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x)                                                               \
  do {                                                                         \
    if (!(x)) {                                                                \
      fprintf(stderr, "Failed line %d: %s\n", __LINE__, #x);                   \
      exit(1);                                                                 \
    }                                                                          \
  } while (0)
static int frames;
static int frame(void *ctx, uint8_t t, const uint8_t *p, size_t n) {
  (void)ctx;
  CHECK(t == 0 && n == 2 && !memcmp(p, "{}", 2));
  frames++;
  return 0;
}
static int images;
static int image(void *ctx, const urs_image *im) {
  (void)ctx;
  CHECK(!strcmp(im->name, "cam"));
  CHECK(im->width == 1 && im->height == 1 && im->size == 4 &&
        im->sequence == 7);
  images++;
  return 0;
}
int main(void) {
  urs_parser p = {0};
  uint8_t h[5], packet[14];
  CHECK(!urs_frame_header(h, 0, 2));
  memcpy(packet, h, 5);
  memcpy(packet + 5, "{}", 2);
  memcpy(packet + 7, packet, 7);
  for (int i = 0; i < 14; i++)
    CHECK(!urs_feed(&p, packet + i, 1, frame, NULL));
  CHECK(frames == 2);
  CHECK(!urs_feed(&p, packet, 14, frame, NULL));
  CHECK(frames == 4);
  urs_parser_destroy(&p);
  uint8_t bad[4] = {0, 0, 0, 0};
  CHECK(urs_feed(&p, bad, 4, frame, NULL) < 0);
  urs_parser_destroy(&p);
  uint8_t im[39] = {2, 1, 0, 0, 7};
  im[16] = 3;
  memcpy(im + 17, "cam", 3);
  im[23] = 1;
  im[25] = 1;
  im[27] = 4;
  im[31] = 4;
  CHECK(!urs_parse_images(im, sizeof(im), image, NULL));
  CHECK(images == 1);
  CHECK(urs_parse_images(im, sizeof(im) - 1, image, NULL) < 0);
  im[20] = 99;
  CHECK(urs_parse_images(im, sizeof(im), image, NULL) < 0);
  uint8_t video[20] = {1, 0, 5};
  video[10] = 1;
  video[12] = 1;
  video[19] = 42;
  urs_image vi = {0};
  vi.width = vi.height = 1;
  vi.data = video;
  vi.size = 20;
  urs_video v;
  CHECK(!urs_parse_video(&vi, &v));
  CHECK(v.epoch == 5 && v.packet_size == 1 && v.packet[0] == 42);
  video[1] = 1;
  CHECK(urs_parse_video(&vi, &v) < 0);
  char json[4096];
  double pos[3] = {1, 2, 3}, q[4] = {0, 0, 0, 2};
  CHECK(urs_camera_json(json, sizeof(json), pos, q) > 0);
  CHECK(strstr(json, "[0,0,0,1]"));
  CHECK(urs_camera_json(json, 8, pos, q) < 0);
  q[3] = 0;
  CHECK(urs_camera_json(json, sizeof(json), pos, q) < 0);
  CHECK(urs_admin_json(json, sizeof(json), "reset", "r\"\\\n", NULL, NULL) > 0);
  CHECK(strstr(json, "r\\\"\\\\\\u000a"));
  CHECK(urs_admin_json(json, sizeof(json), "invalid", "rp0", NULL, NULL) < 0);
  puts("Protocol checks passed");
  return 0;
}
