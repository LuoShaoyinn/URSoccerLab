#ifndef URS_PROTOCOL_H
#define URS_PROTOCOL_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#define URS_MAX_FRAME (32u * 1024u * 1024u)
typedef int (*urs_frame_fn)(void *, uint8_t, const uint8_t *, size_t);
typedef struct {
  uint8_t *buffer;
  size_t used, capacity;
} urs_parser;
/* Callback data is borrowed, valid only during the callback. Nonzero aborts. */
int urs_feed(urs_parser *, const void *, size_t, urs_frame_fn, void *);
void urs_parser_destroy(urs_parser *);
int urs_frame_header(uint8_t out[5], uint8_t type, size_t payload_size);
typedef struct {
  char name[256];
  uint8_t codec, pixel_format, keyframe;
  uint16_t width, height;
  uint32_t sequence, raw_size;
  double sim_time;
  const uint8_t *data;
  size_t size;
} urs_image;
typedef int (*urs_image_fn)(void *, const urs_image *);
int urs_parse_images(const uint8_t *, size_t, urs_image_fn, void *);
typedef struct {
  uint8_t layout;
  uint64_t epoch;
  uint16_t width, height;
  char right_name[256];
  const uint8_t *config, *packet;
  size_t config_size, packet_size;
} urs_video;
int urs_parse_video(const urs_image *, urs_video *);
/* Returns byte count excluding NUL, or -1. Quaternion is normalized. */
int urs_camera_json(char *, size_t, const double position[3],
                    const double xyzw[4]);
int urs_admin_json(char *, size_t, const char *command, const char *actor,
                   const double *position, const double *xyzw);
#ifdef __cplusplus
}
#endif
#endif
