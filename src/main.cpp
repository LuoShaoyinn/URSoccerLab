#include "ursoccerlab/client.hpp"
#define SDL_MAIN_HANDLED
#include <SDL.h>
#include <algorithm>
#include <backends/imgui_impl_sdl2.h>
#include <backends/imgui_impl_sdlrenderer2.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <imgui.h>
static constexpr double pi = 3.141592653589793;
static void rotation(double yaw, double pitch, double q[4]) {
  double a = yaw * .5, b = pitch * .5;
  q[0] = sin(a) * sin(b);
  q[1] = -cos(a) * sin(b);
  q[2] = sin(a) * cos(b);
  q[3] = cos(a) * cos(b);
}
int main(int argc, char **argv) {
  bool smoke = false;
  int rendered = 0;
  bool admin_only = false;
  char host[256] = "127.0.0.1";
  for (int i = 1; i < argc; i++) {
    if (std::string(argv[i]) == "--smoke-test")
      smoke = true;
    else if (std::string(argv[i]) == "--admin-only")
      admin_only = true;
    else if (std::string(argv[i]) == "--host" && i + 1 < argc)
      snprintf(host, sizeof(host), "%s", argv[++i]);
    else {
      fprintf(stderr, "Usage: %s [--host HOST] [--admin-only] [--smoke-test]\n",
              argv[0]);
      return 2;
    }
  }
  SDL_SetMainReady();
  if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) != 0) {
    fprintf(stderr, "SDL: %s\n", SDL_GetError());
    return 1;
  }
  SDL_Window *window =
      SDL_CreateWindow("URSoccerLab Viewer", SDL_WINDOWPOS_CENTERED,
                       SDL_WINDOWPOS_CENTERED, admin_only ? 480 : 1280, 720,
                       SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
  SDL_Renderer *renderer =
      window ? SDL_CreateRenderer(window, -1,
                                  SDL_RENDERER_ACCELERATED |
                                      SDL_RENDERER_PRESENTVSYNC)
             : nullptr;
  if (window && !renderer)
    renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
  if (!renderer) {
    fprintf(stderr, "SDL: %s\n", SDL_GetError());
    if (window)
      SDL_DestroyWindow(window);
    SDL_Quit();
    return 1;
  }
  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGui::GetIO().IniFilename = nullptr;
  ImGui::StyleColorsDark();
  ImGui_ImplSDL2_InitForSDLRenderer(window, renderer);
  ImGui_ImplSDLRenderer2_Init(renderer);
  Client guest, admin;
  int guest_port = 12000, admin_port = 11000;
  bool running = true, captured = false;
  double position[3] = {-4, 0, 2}, yaw = 0, pitch = 0;
  float speed = 3;
  char actor[256] = "robot_rp0";
  float pose[3] = {0, 0, 0}, quat[4] = {0, 0, 0, 1};
  std::string ui_error;
  SDL_Texture *texture = nullptr;
  uint64_t serial = 0, reply_serial = 0;
  int tw = 0, th = 0;
  auto last = std::chrono::steady_clock::now(), last_send = last;
  while (running) {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
      ImGui_ImplSDL2_ProcessEvent(&e);
      if (e.type == SDL_QUIT)
        running = false;
      if (e.type == SDL_WINDOWEVENT &&
          e.window.event == SDL_WINDOWEVENT_FOCUS_LOST) {
        captured = false;
        SDL_SetRelativeMouseMode(SDL_FALSE);
      }
      if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_ESCAPE) {
        captured = false;
        SDL_SetRelativeMouseMode(SDL_FALSE);
      }
      if (captured && e.type == SDL_MOUSEMOTION) {
        yaw -= e.motion.xrel * .0025;
        pitch = std::clamp(pitch - e.motion.yrel * .0025, -pi * .49, pi * .49);
      }
    }
    auto now = std::chrono::steady_clock::now();
    double dt =
        std::min(.05, std::chrono::duration<double>(now - last).count());
    last = now;
    if (captured) {
      const Uint8 *k = SDL_GetKeyboardState(nullptr);
      double v = speed * dt * (k[SDL_SCANCODE_LSHIFT] ? 4 : 1);
      double forward = (double)k[SDL_SCANCODE_W] - k[SDL_SCANCODE_S],
             right = (double)k[SDL_SCANCODE_D] - k[SDL_SCANCODE_A];
      position[0] += v * (forward * cos(yaw) * cos(pitch) + right * sin(yaw));
      position[1] += v * (forward * sin(yaw) * cos(pitch) - right * cos(yaw));
      position[2] += v * (forward * sin(pitch) + k[SDL_SCANCODE_SPACE] -
                          k[SDL_SCANCODE_LCTRL]);
      for (double &p : position)
        p = std::clamp(p, -99.0, 99.0);
    }
    if (guest.alive() &&
        std::chrono::duration<double>(now - last_send).count() >= 1.0 / 30) {
      double q[4];
      rotation(yaw, pitch, q);
      char json[1024];
      int n = urs_camera_json(json, sizeof(json), position, q);
      if (n > 0)
        guest.send(std::string(json, n), true);
      last_send = now;
    }
    Picture image = guest.picture();
    if (!image.rgb.empty() && image.serial != serial) {
      if (!texture || tw != image.width || th != image.height) {
        if (texture)
          SDL_DestroyTexture(texture);
        tw = image.width;
        th = image.height;
        texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGB24,
                                    SDL_TEXTUREACCESS_STREAMING, tw, th);
      }
      if (texture)
        SDL_UpdateTexture(texture, nullptr, image.rgb.data(), tw * 3);
      serial = image.serial;
    }
    ImGui_ImplSDLRenderer2_NewFrame();
    ImGui_ImplSDL2_NewFrame();
    ImGui::NewFrame();
    auto display = ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImVec2(admin_only ? display.x : 350, display.y));
    ImGui::Begin("Controls", nullptr,
                 ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoCollapse);
    ImGui::InputText("Host", host, sizeof(host));
    if (!admin_only) {
      ImGui::SeparatorText("Guest camera");
      ImGui::InputInt("Guest port", &guest_port);
      if (ImGui::Button("Connect guest") && guest_port > 0 &&
          guest_port < 65536) {
        guest.connect(host, guest_port, true);
        serial = 0;
        if (texture) {
          SDL_DestroyTexture(texture);
          texture = nullptr;
        }
      }
      ImGui::SameLine();
      if (ImGui::Button("Close guest")) {
        guest.disconnect();
        captured = false;
        SDL_SetRelativeMouseMode(SDL_FALSE);
      }
      ImGui::TextWrapped("%s", guest.status().c_str());
      ImGui::SliderFloat("Speed (m/s)", &speed, .1f, 15.f);
      ImGui::Text("Position: %.2f %.2f %.2f", position[0], position[1],
                  position[2]);
      if (ImGui::Button("Reset camera")) {
        position[0] = -4;
        position[1] = 0;
        position[2] = 2;
        yaw = pitch = 0;
      }
      ImGui::TextWrapped("Click video to fly. WASD: move; Space/Ctrl: up/down; "
                         "Shift: faster; Esc: release mouse.");
    }
    ImGui::SeparatorText("Administration (optional)");
    ImGui::InputInt("Admin port", &admin_port);
    if (ImGui::Button("Connect admin") && admin_port > 0 && admin_port < 65536)
      admin.connect(host, admin_port, false);
    ImGui::SameLine();
    if (ImGui::Button("Close admin"))
      admin.disconnect();
    ImGui::TextWrapped("%s", admin.status().c_str());
    ImGui::InputText("Actor ID", actor, sizeof(actor));
    Reply reply = admin.reply();
    if (reply.serial != reply_serial) {
      reply_serial = reply.serial;
      if (reply.has_pose && reply.actor == actor) {
        for (int i = 0; i < 3; i++)
          pose[i] = (float)reply.position[i];
        for (int i = 0; i < 4; i++)
          quat[i] = (float)reply.rotation[i];
      }
    }
    ImGui::InputFloat3("Position (m)", pose);
    ImGui::InputFloat4("Rotation xyzw", quat);
    ImGui::BeginDisabled(!admin.alive());
    const char *labels[] = {"Get pose", "Set pose", "Reset actor", "Lock pose",
                            "Unlock pose"};
    const char *commands[] = {"get_pose", "set_pose", "reset", "lock_pose",
                              "unlock_pose"};
    for (int i = 0; i < 5; i++)
      if (ImGui::Button(labels[i])) {
        double p[] = {pose[0], pose[1], pose[2]},
               q[] = {quat[0], quat[1], quat[2], quat[3]};
        char json[4096];
        bool with_pose = i == 1 || i == 3;
        int n =
            urs_admin_json(json, sizeof(json), commands[i], actor,
                           with_pose ? p : nullptr, with_pose ? q : nullptr);
        ui_error = n < 0                              ? "Invalid actor pose"
                   : admin.send(std::string(json, n)) ? ""
                                                      : "Command queue full";
      }
    ImGui::EndDisabled();
    if (!ui_error.empty())
      ImGui::TextWrapped("%s", ui_error.c_str());
    ImGui::TextWrapped("Get pose fills the fields. Set/Lock use "
                       "the fields; joints are unchanged.");
    ImGui::End();
    if (!admin_only) {
      ImGui::SetNextWindowPos(ImVec2(350, 0));
      ImGui::SetNextWindowSize(
          ImVec2(std::max(1.f, display.x - 350), display.y));
      ImGui::Begin("Guest view", nullptr,
                   ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                       ImGuiWindowFlags_NoCollapse);
      if (texture) {
        ImVec2 room = ImGui::GetContentRegionAvail();
        float scale = std::min(room.x / tw, room.y / th);
        if (scale > 0) {
          ImGui::Image((ImTextureID)(intptr_t)texture,
                       ImVec2(tw * scale, th * scale));
          if (ImGui::IsItemClicked() && guest.alive()) {
            captured = SDL_SetRelativeMouseMode(SDL_TRUE) == 0;
            if (!captured)
              ui_error = SDL_GetError();
          }
        }
      } else
        ImGui::TextWrapped(
            "Connect to a running simulator with guest_inspector enabled. "
            "Video starts at a periodic keyframe.");
      ImGui::End();
    }
    ImGui::Render();
    SDL_SetRenderDrawColor(renderer, 12, 12, 12, 255);
    SDL_RenderClear(renderer);
    ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData(), renderer);
    SDL_RenderPresent(renderer);
    if (smoke && ++rendered >= 3)
      running = false;
    double elapsed =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - now)
            .count();
    if (elapsed < 1.0 / 60)
      SDL_Delay((Uint32)((1.0 / 60 - elapsed) * 1000));
  }
  guest.disconnect();
  admin.disconnect();
  if (texture)
    SDL_DestroyTexture(texture);
  ImGui_ImplSDLRenderer2_Shutdown();
  ImGui_ImplSDL2_Shutdown();
  ImGui::DestroyContext();
  SDL_DestroyRenderer(renderer);
  SDL_DestroyWindow(window);
  SDL_Quit();
  return 0;
}
