#pragma once
#include <array>
#include <cstdint>
#include <string>
struct Reply {
  bool valid = false, ok = false, has_pose = false;
  std::string operation, actor, error;
  std::array<double, 3> position{};
  std::array<double, 4> rotation{};
  uint64_t serial = 0;
};
Reply parse_reply(const char *data, size_t size);
