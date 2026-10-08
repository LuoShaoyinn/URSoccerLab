#include "ursoccerlab/reply.hpp"
#include <cJSON.h>
#include <cmath>
#include <cstdint>
#include <memory>
Reply parse_reply(const char *data, size_t size) {
  Reply reply;
  std::string text(data, size);
  std::unique_ptr<cJSON, decltype(&cJSON_Delete)> root(
      cJSON_ParseWithLengthOpts(text.c_str(), text.size() + 1, nullptr, 1),
      cJSON_Delete);
  if (!root || !cJSON_IsObject(root.get()))
    return reply;
  auto field = [&](const char *key) {
    return cJSON_GetObjectItemCaseSensitive(root.get(), key);
  };
  auto string = [&](const char *key) {
    const auto value = field(key);
    return cJSON_IsString(value)
               ? std::string(value->valuestring).substr(0, 255)
               : std::string();
  };
  auto ok = field("ok");
  if (!cJSON_IsBool(ok))
    return reply;
  reply.valid = true;
  reply.ok = cJSON_IsTrue(ok);
  reply.operation = string("op");
  if (reply.operation.empty())
    reply.operation = string("command");
  reply.actor = string("actor_id");
  reply.error = string("message");
  if (reply.error.empty())
    reply.error = string("error");
  auto array = [&](const char *key, double *out, int count) {
    auto value = field(key);
    if (!cJSON_IsArray(value) || cJSON_GetArraySize(value) != count)
      return false;
    for (int i = 0; i < count; i++) {
      auto number = cJSON_GetArrayItem(value, i);
      if (!cJSON_IsNumber(number) || !std::isfinite(number->valuedouble))
        return false;
      out[i] = number->valuedouble;
    }
    return true;
  };
  if (reply.ok) {
    reply.has_pose = array("translation_m", reply.position.data(), 3) &&
                     array("rotation_quat_xyzw", reply.rotation.data(), 4);
    if (!reply.has_pose)
      reply.has_pose =
          array("applied_translation_m", reply.position.data(), 3) &&
          array("applied_rotation_quat_xyzw", reply.rotation.data(), 4);
  }
  return reply;
}
