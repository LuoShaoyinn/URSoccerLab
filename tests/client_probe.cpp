#include "ursoccerlab/client.hpp"
#include <chrono>
#include <cstdio>
#include <cstdlib>
int main(int argc, char **argv) {
  if (argc != 2)
    return 2;
  Client c;
  c.connect("127.0.0.1", atoi(argv[1]), true);
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
  bool sent = false;
  while (std::chrono::steady_clock::now() < deadline) {
    if (c.alive() && !sent) {
      double p[] = {-4, 0, 2}, q[] = {0, 0, 0, 1};
      char j[1024];
      int n = urs_camera_json(j, sizeof(j), p, q);
      sent = c.send(std::string(j, n), true);
    }
    Picture p = c.picture();
    if (p.serial) {
      // Fixture visible crop is red on the left, blue on the right.
      auto left =
          p.rgb.data() + ((size_t)(p.height / 2) * p.width + p.width / 4) * 3;
      auto right = p.rgb.data() +
                   ((size_t)(p.height / 2) * p.width + 3 * p.width / 4) * 3;
      if (p.width != 120 || p.height != 60 || left[0] < 180 || right[2] < 180) {
        fprintf(stderr, "Unexpected decoded frame\n");
        return 1;
      }
      c.disconnect();
      puts("Camera command and cropped video decoded");
      return 0;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  fprintf(stderr, "Timed out: %s\n", c.status().c_str());
  return 1;
}
