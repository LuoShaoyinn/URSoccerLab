#include "ursoccerlab/client.hpp"
extern "C" {
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
}
#include <algorithm>
#include <chrono>
#include <cstring>
#include <memory>
#include <stdexcept>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
using Socket = SOCKET;
static void close_socket(Socket s) { closesocket(s); }
static bool pending() {
  int e = WSAGetLastError();
  return e == WSAEWOULDBLOCK || e == WSAEINPROGRESS;
}
static void nonblock(Socket s) {
  u_long v = 1;
  ioctlsocket(s, FIONBIO, &v);
}
static constexpr Socket bad = INVALID_SOCKET;
#else
#include <cerrno>
#include <fcntl.h>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>
using Socket = int;
static void close_socket(Socket s) { close(s); }
static bool pending() {
  return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINPROGRESS;
}
static void nonblock(Socket s) {
  fcntl(s, F_SETFL, fcntl(s, F_GETFL) | O_NONBLOCK);
}
static constexpr Socket bad = -1;
#endif
Client::Decoder::~Decoder() { avcodec_free_context(&ctx); }
Client::~Client() { disconnect(); }
void Client::disconnect() {
  stop = true;
  if (worker.joinable())
    worker.join();
  active = false;
  decoders.clear();
}
void Client::connect(std::string host, int port, bool video) {
  disconnect();
  {
    std::lock_guard<std::mutex> l(mutex);
    commands.clear();
    camera_command.clear();
    latest = {};
    latest_reply = {};
    message = "Connecting...";
  }
  stop = false;
  wants_video = video;
  worker = std::thread(&Client::run, this, std::move(host), port);
}
bool Client::send(std::string j, bool camera) {
  if (!active)
    return false;
  std::lock_guard<std::mutex> l(mutex);
  if (camera)
    camera_command = std::move(j);
  else {
    if (commands.size() >= 32)
      return false;
    commands.push_back(std::move(j));
  }
  return true;
}
void Client::set_status(std::string s) {
  std::lock_guard<std::mutex> l(mutex);
  message = std::move(s);
}
std::string Client::status() {
  std::lock_guard<std::mutex> l(mutex);
  return message;
}
Picture Client::picture() {
  std::lock_guard<std::mutex> l(mutex);
  return latest;
}
Reply Client::reply() {
  std::lock_guard<std::mutex> l(mutex);
  return latest_reply;
}
void Client::run(std::string host, int port) {
#ifdef _WIN32
  WSADATA w;
  if (WSAStartup(MAKEWORD(2, 2), &w)) {
    set_status("Winsock startup failed");
    return;
  }
#endif
  Socket sock = bad;
  urs_parser parser{};
  try {
    addrinfo hints{};
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_family = AF_UNSPEC;
    addrinfo *addresses = nullptr;
    std::string service = std::to_string(port);
    if (getaddrinfo(host.c_str(), service.c_str(), &hints, &addresses))
      throw std::runtime_error("Cannot resolve host");
    std::unique_ptr<addrinfo, decltype(&freeaddrinfo)> address_guard(
        addresses, freeaddrinfo);
    for (auto a = addresses; a && !stop; a = a->ai_next) {
      sock = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
      if (sock == bad)
        continue;
      nonblock(sock);
      int rc = ::connect(sock, a->ai_addr, (int)a->ai_addrlen);
      if (rc != 0 && pending()) {
        auto deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (!stop && std::chrono::steady_clock::now() < deadline) {
          fd_set write, error;
          FD_ZERO(&write);
          FD_ZERO(&error);
          FD_SET(sock, &write);
          FD_SET(sock, &error);
          timeval tv{0, 20000};
          rc = select((int)sock + 1, nullptr, &write, &error, &tv);
          if (rc > 0) {
            int e = 0;
#ifdef _WIN32
            int len = sizeof(e);
#else
            socklen_t len = sizeof(e);
#endif
            getsockopt(sock, SOL_SOCKET, SO_ERROR, (char *)&e, &len);
            rc = e ? -1 : 0;
            break;
          }
          rc = -1;
        }
      }
      if (rc == 0 && !stop)
        break;
      close_socket(sock);
      sock = bad;
    }
    if (sock == bad || stop)
      throw std::runtime_error("Connection failed or timed out");
    active = true;
    set_status(wants_video ? "Connected; waiting for video keyframe"
                           : "Admin connected");
    std::vector<uint8_t> outgoing;
    size_t sent = 0;
    uint8_t bytes[131072];
    while (!stop) {
      if (sent == outgoing.size()) {
        outgoing.clear();
        sent = 0;
        std::string j;
        {
          std::lock_guard<std::mutex> l(mutex);
          if (!commands.empty()) {
            j = std::move(commands.front());
            commands.pop_front();
          } else
            j.swap(camera_command);
        }
        if (!j.empty()) {
          uint8_t h[5];
          if (urs_frame_header(h, 0, j.size()))
            throw std::runtime_error("Command too large");
          outgoing.insert(outgoing.end(), h, h + 5);
          outgoing.insert(outgoing.end(), j.begin(), j.end());
        }
      }
      if (sent < outgoing.size()) {
#ifdef _WIN32
        int n = ::send(sock, (const char *)outgoing.data() + sent,
                       (int)(outgoing.size() - sent), 0);
#else
        int n = ::send(sock, outgoing.data() + sent, outgoing.size() - sent,
                       MSG_NOSIGNAL);
#endif
        if (n > 0)
          sent += (size_t)n;
        else if (n == 0 || !pending())
          throw std::runtime_error("Send failed");
      }
      int n = recv(sock, (char *)bytes, sizeof(bytes), 0);
      if (n > 0) {
        if (urs_feed(&parser, bytes, (size_t)n, frame, this))
          throw std::runtime_error("Invalid server message");
      } else if (n == 0)
        throw std::runtime_error("Server disconnected");
      else if (!pending())
        throw std::runtime_error("Receive failed");
      else
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    set_status("Disconnected");
  } catch (const std::exception &e) {
    set_status(e.what());
  }
  active = false;
  if (sock != bad)
    close_socket(sock);
  urs_parser_destroy(&parser);
#ifdef _WIN32
  WSACleanup();
#endif
}
int Client::frame(void *ctx, uint8_t type, const uint8_t *p, size_t n) {
  auto self = (Client *)ctx;
  if (type == 0) {
    Reply reply = parse_reply((const char *)p, n);
    if (!reply.valid) {
      self->set_status("Invalid server reply");
      return 0;
    }
    // Successful per-camera acknowledgements arrive at 30 Hz. They should not
    // alter layout or overwrite connection/video status on every update.
    if (self->wants_video) {
      if (!reply.ok)
        self->set_status("Camera rejected: " + reply.error);
      return 0;
    }
    std::lock_guard<std::mutex> lock(self->mutex);
    reply.serial = self->latest_reply.serial + 1;
    self->latest_reply = reply;
    if (!reply.ok)
      self->message = "Request failed: " + reply.error;
    else if (reply.operation == "get_pose")
      self->message = "Pose received";
    else if (reply.operation == "set_pose")
      self->message = "Pose updated";
    else if (reply.operation == "reset")
      self->message = "Actor reset";
    else if (reply.operation == "lock_pose")
      self->message = "Pose locked";
    else if (reply.operation == "unlock_pose")
      self->message = "Pose unlocked";
    else
      self->message = "Request completed";
    return 0;
  }
  if (type == 1 && self->wants_video)
    return urs_parse_images(p, n, image, self);
  return 0;
}
int Client::image(void *ctx, const urs_image *im) {
  return ((Client *)ctx)->decode(im);
}
int Client::decode(const urs_image *im) {
  if (im->pixel_format != 0)
    return -1;
  if (im->codec == 0) {
    if (im->size != (size_t)im->width * im->height * 4)
      return -1;
    Picture p;
    p.width = im->width;
    p.height = im->height;
    p.rgb.resize((size_t)p.width * p.height * 3);
    for (size_t i = 0; i < im->size / 4; i++) {
      p.rgb[i * 3] = im->data[i * 4 + 2];
      p.rgb[i * 3 + 1] = im->data[i * 4 + 1];
      p.rgb[i * 3 + 2] = im->data[i * 4];
    }
    std::lock_guard<std::mutex> l(mutex);
    p.serial = latest.serial + 1;
    latest = std::move(p);
    message = "Streaming";
    return 0;
  }
  if (im->codec != 1 && im->codec != 3 && im->codec != 4 && im->codec != 5)
    return -1;
  urs_video video{};
  bool compressed = im->codec != 1;
  if (compressed && urs_parse_video(im, &video))
    return -1;
  auto &d = decoders[im->name];
  if (d.codec != im->codec || d.epoch != video.epoch ||
      (compressed && d.seen && im->sequence != d.last + 1))
    avcodec_free_context(&d.ctx);
  d.codec = im->codec;
  d.epoch = video.epoch;
  d.last = im->sequence;
  d.seen = true;
  if (!d.ctx) {
    if (compressed && !im->keyframe)
      return 0;
    AVCodecID id = im->codec == 3   ? AV_CODEC_ID_AV1
                   : im->codec == 4 ? AV_CODEC_ID_H264
                   : im->codec == 5 ? AV_CODEC_ID_HEVC
                                    : AV_CODEC_ID_MJPEG;
    const AVCodec *codec = id == AV_CODEC_ID_AV1
                               ? avcodec_find_decoder_by_name("libdav1d")
                               : avcodec_find_decoder(id);
    if (!codec)
      codec = avcodec_find_decoder(id);
    if (!codec) {
      set_status("Required FFmpeg decoder unavailable");
      return -1;
    }
    d.ctx = avcodec_alloc_context3(codec);
    if (!d.ctx)
      return -1;
    d.ctx->thread_count = 1;
    d.ctx->max_pixels = 8192LL * 8192;
    if (video.config_size) {
      d.ctx->extradata = (uint8_t *)av_mallocz(video.config_size +
                                               AV_INPUT_BUFFER_PADDING_SIZE);
      if (!d.ctx->extradata)
        return -1;
      memcpy(d.ctx->extradata, video.config, video.config_size);
      d.ctx->extradata_size = (int)video.config_size;
    }
    if (avcodec_open2(d.ctx, codec, nullptr) < 0) {
      avcodec_free_context(&d.ctx);
      return -1;
    }
  }
  AVPacket *packet = av_packet_alloc();
  if (!packet)
    return -1;
  size_t size = compressed ? video.packet_size : im->size;
  const uint8_t *data = compressed ? video.packet : im->data;
  if (av_new_packet(packet, (int)size) < 0) {
    av_packet_free(&packet);
    return -1;
  }
  memcpy(packet->data, data, size);
  packet->pts = im->sequence;
  int rc = avcodec_send_packet(d.ctx, packet);
  av_packet_free(&packet);
  if (rc < 0) {
    avcodec_free_context(&d.ctx);
    return 0;
  }
  AVFrame *f = av_frame_alloc();
  if (!f)
    return -1;
  while ((rc = avcodec_receive_frame(d.ctx, f)) >= 0) {
    if (f->width < im->width || f->height < im->height ||
        (compressed && (f->width > video.width || f->height > video.height))) {
      av_frame_free(&f);
      return -1;
    }
    SwsContext *sws = sws_getContext(
        f->width, f->height, (AVPixelFormat)f->format, f->width, f->height,
        AV_PIX_FMT_RGB24, SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);
    if (!sws) {
      av_frame_free(&f);
      return -1;
    }
    std::vector<uint8_t> full((size_t)f->width * f->height * 3);
    uint8_t *dst[] = {full.data()};
    int stride[] = {f->width * 3};
    sws_scale(sws, f->data, f->linesize, 0, f->height, dst, stride);
    sws_freeContext(sws);
    Picture p;
    p.width = im->width;
    p.height = im->height;
    p.rgb.resize((size_t)p.width * p.height * 3);
    for (int y = 0; y < p.height; y++)
      memcpy(p.rgb.data() + (size_t)y * p.width * 3,
             full.data() + (size_t)y * f->width * 3, (size_t)p.width * 3);
    {
      std::lock_guard<std::mutex> l(mutex);
      p.serial = latest.serial + 1;
      latest = std::move(p);
      message = "Streaming";
    }
    av_frame_unref(f);
  }
  av_frame_free(&f);
  if (rc != AVERROR(EAGAIN) && rc != AVERROR_EOF)
    avcodec_free_context(&d.ctx);
  return 0;
}
