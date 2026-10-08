#pragma once
#include "reply.hpp"
#include "urs_protocol.h"
#include <atomic>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
struct AVCodecContext;

struct Picture {
  int width = 0, height = 0;
  std::vector<unsigned char> rgb;
  uint64_t serial = 0;
};
class Client {
public:
  ~Client();
  void connect(std::string host, int port, bool video);
  void disconnect();
  bool send(std::string json, bool camera = false);
  std::string status();
  Picture picture();
  Reply reply();
  bool alive() const { return active; }

private:
  struct Decoder {
    AVCodecContext *ctx = nullptr;
    uint64_t epoch = 0;
    uint32_t last = 0;
    int codec = -1;
    bool seen = false;
    ~Decoder();
  };
  std::thread worker;
  std::atomic<bool> stop{false}, active{false};
  std::mutex mutex;
  std::string message = "Disconnected";
  Picture latest;
  Reply latest_reply;
  std::deque<std::string> commands;
  std::string camera_command;
  std::map<std::string, Decoder> decoders;
  bool wants_video = false;
  void run(std::string, int);
  void set_status(std::string);
  static int frame(void *, uint8_t, const uint8_t *, size_t);
  static int image(void *, const urs_image *);
  int decode(const urs_image *);
};
