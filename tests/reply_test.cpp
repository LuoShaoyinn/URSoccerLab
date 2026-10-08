#include "ursoccerlab/reply.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#define CHECK(x)                                                               \
  do {                                                                         \
    if (!(x)) {                                                                \
      fprintf(stderr, "Failed line %d: %s\n", __LINE__, #x);                   \
      exit(1);                                                                 \
    }                                                                          \
  } while (0)
static Reply parse(const char *s) { return parse_reply(s, strlen(s)); }
int main() {
  auto r = parse(
      R"({"ok":true,"op":"get_pose","actor_id":"robot_rp0","translation_m":[1,2,3],"rotation_quat_xyzw":[0,0,0,1],"joint_qpos":[1,2]})");
  CHECK(r.valid && r.ok && r.has_pose && r.position[1] == 2 &&
        r.rotation[3] == 1);
  CHECK(r.actor == "robot_rp0" && r.operation == "get_pose");
  r = parse(
      R"({"ok":true,"op":"set_pose","applied_translation_m":[4,5,6],"applied_rotation_quat_xyzw":[0,0,1,0]})");
  CHECK(r.has_pose && r.position[0] == 4);
  r = parse(
      R"({"ok":false,"error":"actor_not_found","message":"Unknown actor"})");
  CHECK(r.valid && !r.ok && !r.has_pose && r.error == "Unknown actor");
  r = parse(R"({"ok":true,"command":"set_camera"})");
  CHECK(r.ok && !r.has_pose && r.operation == "set_camera");
  CHECK(!parse("{bad}").valid);
  CHECK(!parse("{\"ok\":true} junk").valid);
  CHECK(!parse("{\"ok\":\"true\"}").valid);
  r = parse(
      R"({"ok":true,"translation_m":[1,2],"rotation_quat_xyzw":[0,0,0,1]})");
  CHECK(!r.has_pose);
  puts("Readable reply checks passed");
}
