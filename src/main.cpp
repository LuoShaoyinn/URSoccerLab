#include "ursoccerlab/client.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <wx/dcbuffer.h>
#include <wx/scrolwin.h>
#include <wx/spinctrl.h>
#include <wx/splitter.h>
#include <wx/statline.h>
#include <wx/timer.h>
#include <wx/wx.h>

static constexpr double pi = 3.141592653589793;
static void rotation(double yaw, double pitch, double q[4]) {
  double a = yaw * .5, b = pitch * .5;
  q[0] = sin(a) * sin(b);
  q[1] = -cos(a) * sin(b);
  q[2] = sin(a) * cos(b);
  q[3] = cos(a) * cos(b);
}

class VideoPanel : public wxPanel {
public:
  explicit VideoPanel(wxWindow *parent)
      : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                wxWANTS_CHARS) {}
  bool AcceptsFocus() const override { return true; }
};

class Viewer : public wxFrame {
  Client guest, admin;
  wxPanel *video = nullptr;
  wxTextCtrl *host, *actor, *pose[3], *quat[4];
  wxSpinCtrl *guest_port = nullptr, *admin_port;
  wxButton *guest_connect = nullptr, *admin_connect = nullptr;
  wxStaticText *guest_status = nullptr, *admin_status, *rx_label = nullptr,
               *tx_label = nullptr, *error;
  std::vector<wxWindow *> guest_controls, admin_controls;
  const double camera_speed = 3;
  uint64_t last_rx = 0, last_tx = 0;
  bool was_guest_connected = false;
  std::chrono::steady_clock::time_point last_frame =
                                            std::chrono::steady_clock::now(),
                                        last_rates = last_frame;
  wxBitmap bitmap;
  wxTimer timer;
  bool captured = false;
  std::array<bool, 512> keys{};
  double position[3] = {-4, 0, 2}, yaw = 0, pitch = 0;
  uint64_t serial = 0, reply_serial = 0;
  std::chrono::steady_clock::time_point last = std::chrono::steady_clock::now(),
                                        last_send = last;

  void release_mouse() {
    if (!captured)
      return;
    captured = false;
    keys.fill(false);
    if (video->HasCapture())
      video->ReleaseMouse();
    video->SetCursor(wxNullCursor);
  }
  void recenter() {
    wxSize s = video->GetClientSize();
    video->WarpPointer(s.x / 2, s.y / 2);
  }
  void tick(wxTimerEvent &) {
    auto now = std::chrono::steady_clock::now();
    double dt =
        std::min(.05, std::chrono::duration<double>(now - last).count());
    last = now;
    if (captured && !guest.alive())
      release_mouse();
    if (captured) {
      double v = camera_speed * dt * (keys[WXK_CONTROL] ? 4 : 1);
      double f = (int)keys['W'] - (int)keys['S'];
      double r = (int)keys['D'] - (int)keys['A'];
      position[0] += v * (f * cos(yaw) + r * sin(yaw));
      position[1] += v * (f * sin(yaw) - r * cos(yaw));
      position[2] += v * ((int)keys[WXK_SPACE] - (int)keys[WXK_SHIFT]);
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
    if (video) {
      Picture image = guest.picture();
      if (!image.rgb.empty() && image.serial != serial) {
        wxImage frame(image.width, image.height);
        if (frame.IsOk() &&
            image.rgb.size() == (size_t)image.width * image.height * 3) {
          std::memcpy(frame.GetData(), image.rgb.data(), image.rgb.size());
          bitmap = wxBitmap(frame);
          video->Refresh(false);
        }
        serial = image.serial;
        last_frame = now;
      }
      bool connected = guest.alive();
      if (connected && !was_guest_connected)
        last_frame = now;
      was_guest_connected = connected;
      guest_connect->SetLabel(guest.connecting() ? "Connecting..."
                              : connected        ? "Disconnect"
                                                 : "Connect");
      guest_connect->Enable(!guest.connecting());
      guest_port->Enable(!connected && !guest.connecting());
      for (auto *control : guest_controls)
        control->Enable(connected);
      wxString status = wxString::FromUTF8(guest.status());
      if (connected &&
          std::chrono::duration<double>(now - last_frame).count() >= 10)
        status = "No video for 10 seconds";
      if (status == "Disconnected")
        status.clear();
      guest_status->SetLabel(status);
      double elapsed = std::chrono::duration<double>(now - last_rates).count();
      if (elapsed >= 1) {
        uint64_t rx = guest.received_bytes(), tx = guest.sent_bytes();
        rx_label->SetLabel(wxString::Format(
            "RX %.1f kbit/s",
            (rx >= last_rx ? rx - last_rx : rx) * 8.0 / 1000 / elapsed));
        tx_label->SetLabel(wxString::Format(
            "TX %.1f kbit/s",
            (tx >= last_tx ? tx - last_tx : tx) * 8.0 / 1000 / elapsed));
        last_rx = rx;
        last_tx = tx;
        last_rates = now;
      }
    }
    bool admin_connected = admin.alive();
    admin_connect->SetLabel(admin.connecting() ? "Connecting..."
                            : admin_connected  ? "Disconnect"
                                               : "Connect");
    admin_connect->Enable(!admin.connecting());
    admin_port->Enable(!admin_connected && !admin.connecting());
    host->Enable(!admin_connected && !admin.connecting() &&
                 (!video || (!guest.alive() && !guest.connecting())));
    for (auto *control : admin_controls)
      control->Enable(admin_connected);
    wxString admin_message = wxString::FromUTF8(admin.status());
    admin_status->SetLabel(admin_message == "Disconnected" ? ""
                                                           : admin_message);
    Reply reply = admin.reply();
    if (reply.serial != reply_serial) {
      reply_serial = reply.serial;
      if (reply.has_pose &&
          wxString::FromUTF8(reply.actor) == actor->GetValue()) {
        for (int i = 0; i < 3; ++i)
          pose[i]->ChangeValue(wxString::Format("%.6g", reply.position[i]));
        for (int i = 0; i < 4; ++i)
          quat[i]->ChangeValue(wxString::Format("%.6g", reply.rotation[i]));
      }
    }
  }
  wxTextCtrl *field(wxWindow *parent, wxBoxSizer *sizer, const wxString &label,
                    const wxString &value) {
    auto *row = new wxBoxSizer(wxHORIZONTAL);
    row->Add(new wxStaticText(parent, wxID_ANY, label), 0,
             wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(6));
    auto *out = new wxTextCtrl(parent, wxID_ANY, value);
    row->Add(out, 1, wxEXPAND);
    sizer->Add(row, 0, wxEXPAND | wxBOTTOM, FromDIP(6));
    return out;
  }
  void command(int index) {
    static const char *commands[] = {"get_pose", "set_pose", "reset"};
    double p[3], q[4];
    bool with_pose = index == 1;
    if (with_pose) {
      for (int i = 0; i < 3; ++i)
        if (!pose[i]->GetValue().ToDouble(&p[i]) || !std::isfinite(p[i])) {
          error->SetLabel("Invalid position");
          return;
        }
      for (int i = 0; i < 4; ++i)
        if (!quat[i]->GetValue().ToDouble(&q[i]) || !std::isfinite(q[i])) {
          error->SetLabel("Invalid rotation");
          return;
        }
    }
    char json[4096];
    std::string id = actor->GetValue().ToStdString(wxConvUTF8);
    int n = urs_admin_json(json, sizeof(json), commands[index], id.c_str(),
                           with_pose ? p : nullptr, with_pose ? q : nullptr);
    error->SetLabel(n < 0                              ? "Invalid actor pose"
                    : admin.send(std::string(json, n)) ? ""
                                                       : "Command queue full");
  }

public:
  Viewer(bool admin_only, const wxString &initial_host)
      : wxFrame(nullptr, wxID_ANY, "URSoccerLab Viewer", wxDefaultPosition,
                wxSize(admin_only ? 600 : 1280, 720)),
        timer(this) {
    auto *root = new wxBoxSizer(wxHORIZONTAL);
    wxSplitterWindow *splitter =
        admin_only ? nullptr
                   : new wxSplitterWindow(this, wxID_ANY, wxDefaultPosition,
                                          wxDefaultSize, wxSP_LIVE_UPDATE);
    auto *panel = new wxScrolledWindow(
        splitter ? static_cast<wxWindow *>(splitter)
                 : static_cast<wxWindow *>(this),
        wxID_ANY, wxDefaultPosition, wxDefaultSize, wxVSCROLL | wxHSCROLL);
    panel->SetScrollRate(FromDIP(12), FromDIP(12));
    auto *controls = new wxBoxSizer(wxVERTICAL);
    host = field(panel, controls, "Host", initial_host);
    auto section = [&](const char *title) {
      controls->Add(new wxStaticLine(panel), 0, wxEXPAND | wxTOP | wxBOTTOM,
                    FromDIP(6));
      controls->Add(new wxStaticText(panel, wxID_ANY, title), 0, wxBOTTOM,
                    FromDIP(5));
    };
    auto port_row = [&](wxSpinCtrl *&port, wxButton *&connect, int value) {
      auto *row = new wxBoxSizer(wxHORIZONTAL);
      row->Add(new wxStaticText(panel, wxID_ANY, "Port:"), 0,
               wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(5));
      port = new wxSpinCtrl(panel, wxID_ANY);
      port->SetRange(1, 65535);
      port->SetValue(value);
      row->Add(port, 1, wxRIGHT, FromDIP(5));
      connect = new wxButton(panel, wxID_ANY, "Connect");
      row->Add(connect, 0);
      controls->Add(row, 0, wxEXPAND | wxBOTTOM, FromDIP(5));
    };
    auto number_row = [&](const char *label, wxTextCtrl **fields, int count,
                          const char *const *values) {
      auto *row = new wxBoxSizer(wxHORIZONTAL);
      row->Add(new wxStaticText(panel, wxID_ANY, label), 0,
               wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(5));
      for (int i = 0; i < count; ++i) {
        fields[i] = new wxTextCtrl(panel, wxID_ANY, values[i],
                                   wxDefaultPosition, wxSize(FromDIP(30), -1));
        row->Add(fields[i], 1, wxRIGHT, FromDIP(3));
      }
      controls->Add(row, 0, wxEXPAND | wxBOTTOM, FromDIP(5));
      return row;
    };
    if (!admin_only) {
      section("Guest");
      port_row(guest_port, guest_connect, 12000);
      guest_connect->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) {
        if (guest.alive()) {
          release_mouse();
          guest.disconnect();
        } else {
          release_mouse();
          guest.connect(host->GetValue().ToStdString(wxConvUTF8),
                        guest_port->GetValue(), true);
          serial = 0;
          last_rx = last_tx = 0;
          bitmap = wxBitmap();
          video->Refresh();
        }
      });
      auto *traffic_row = new wxBoxSizer(wxHORIZONTAL);
      rx_label = new wxStaticText(panel, wxID_ANY, "RX 0.0 kbit/s");
      tx_label = new wxStaticText(panel, wxID_ANY, "TX 0.0 kbit/s");
      traffic_row->Add(rx_label, 1, wxEXPAND);
      traffic_row->Add(tx_label, 1, wxEXPAND);
      controls->Add(traffic_row, 0, wxEXPAND | wxBOTTOM, FromDIP(5));
      auto *reset = new wxButton(panel, wxID_ANY, "Reset camera");
      controls->Add(reset, 0, wxEXPAND | wxBOTTOM, FromDIP(5));
      guest_controls.push_back(reset);
      reset->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) {
        position[0] = -4;
        position[1] = 0;
        position[2] = 2;
        yaw = pitch = 0;
      });
      guest_status = new wxStaticText(panel, wxID_ANY, "");
      controls->Add(guest_status, 0, wxBOTTOM, FromDIP(5));
      auto *help =
          new wxStaticText(panel, wxID_ANY,
                           "Click video to fly. WASD: level movement; Space: "
                           "up; Shift: down; Ctrl: faster; Esc: release.");
      help->Wrap(FromDIP(260));
      controls->Add(help, 0, wxBOTTOM, FromDIP(5));
    }
    section("Admin");
    port_row(admin_port, admin_connect, 11000);
    admin_connect->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) {
      if (admin.alive())
        admin.disconnect();
      else
        admin.connect(host->GetValue().ToStdString(wxConvUTF8),
                      admin_port->GetValue(), false);
    });
    admin_status = new wxStaticText(panel, wxID_ANY, "");
    controls->Add(admin_status, 0, wxBOTTOM, FromDIP(5));
    actor = field(panel, controls, "Actor ID:", "robot_rp0");
    admin_controls.push_back(actor);
    const char *position_values[] = {"0", "0", "0"};
    const char *rotation_values[] = {"0", "0", "0", "1"};
    number_row("Position:", pose, 3, position_values);
    number_row("Rotation:", quat, 4, rotation_values);
    for (auto *f : pose)
      admin_controls.push_back(f);
    for (auto *f : quat)
      admin_controls.push_back(f);
    const char *labels[] = {"Get pose", "Set pose", "Reset actor"};
    auto *buttons = new wxBoxSizer(wxHORIZONTAL);
    controls->Add(buttons, 0, wxEXPAND);
    for (int i = 0; i < 3; ++i) {
      auto *b = new wxButton(panel, wxID_ANY, labels[i], wxDefaultPosition,
                             wxDefaultSize, wxBU_EXACTFIT);
      buttons->Add(b, 1, wxEXPAND | wxBOTTOM | (i < 2 ? wxRIGHT : 0),
                   FromDIP(5));
      admin_controls.push_back(b);
      b->Bind(wxEVT_BUTTON, [this, i](wxCommandEvent &) {
        if (admin.alive())
          command(i);
      });
    }
    error = new wxStaticText(panel, wxID_ANY, "");
    controls->Add(error);
    for (auto *c : guest_controls)
      c->Enable(false);
    for (auto *c : admin_controls)
      c->Enable(false);
    controls->SetMinSize(FromDIP(300), -1);
    auto *padded_controls = new wxBoxSizer(wxVERTICAL);
    padded_controls->Add(controls, 1, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(8));
    panel->SetSizer(padded_controls);
    if (admin_only)
      root->Add(panel, 1, wxEXPAND | wxALL, FromDIP(8));
    if (!admin_only) {
      auto *toggle = new wxButton(this, wxID_ANY, "<", wxDefaultPosition,
                                  wxSize(FromDIP(26), -1), wxBU_EXACTFIT);
      toggle->SetToolTip("Hide or show controls");
      root->Add(toggle, 0, wxALIGN_TOP);
      root->Add(splitter, 1, wxEXPAND);
      video = new VideoPanel(splitter);
      video->SetBackgroundStyle(wxBG_STYLE_PAINT);
      video->SetBackgroundColour(*wxBLACK);
      splitter->SetMinimumPaneSize(1);
      splitter->SetSashGravity(0.25);
      splitter->SplitVertically(panel, video, FromDIP(320));
      CallAfter([splitter] {
        splitter->SetSashPosition(std::max(1, splitter->GetClientSize().x / 4));
      });
      splitter->Bind(
          wxEVT_SPLITTER_SASH_POS_CHANGING, [splitter](wxSplitterEvent &event) {
            event.SetSashPosition(
                std::max(event.GetSashPosition(),
                         std::max(1, splitter->GetClientSize().x / 4)));
          });
      splitter->Bind(wxEVT_SIZE, [splitter](wxSizeEvent &event) {
        event.Skip();
        splitter->CallAfter([splitter] {
          if (splitter->IsSplit()) {
            int minimum = std::max(1, splitter->GetClientSize().x / 4);
            if (splitter->GetSashPosition() < minimum)
              splitter->SetSashPosition(minimum);
          }
        });
      });
      toggle->Bind(
          wxEVT_BUTTON, [splitter, panel, this, toggle](wxCommandEvent &) {
            if (splitter->IsSplit()) {
              splitter->Unsplit(panel);
              toggle->SetLabel(">");
            } else {
              panel->Show();
              splitter->SplitVertically(
                  panel, video, std::max(1, splitter->GetClientSize().x / 4));
              toggle->SetLabel("<");
            }
          });
      video->Bind(wxEVT_PAINT, [this](wxPaintEvent &) {
        wxAutoBufferedPaintDC dc(video);
        dc.SetBackground(*wxBLACK_BRUSH);
        dc.Clear();
        if (bitmap.IsOk()) {
          wxSize s = video->GetClientSize();
          double k = std::min((double)s.x / bitmap.GetWidth(),
                              (double)s.y / bitmap.GetHeight());
          int w = std::max(1, (int)(bitmap.GetWidth() * k)),
              h = std::max(1, (int)(bitmap.GetHeight() * k));
          wxBitmap scaled(
              bitmap.ConvertToImage().Scale(w, h, wxIMAGE_QUALITY_NORMAL));
          dc.DrawBitmap(scaled, (s.x - w) / 2, (s.y - h) / 2);
        } else {
          dc.SetTextForeground(*wxWHITE);
          dc.DrawText(
              "Connect to a guest camera. Video starts at a periodic keyframe.",
              FromDIP(10), FromDIP(10));
        }
      });
      video->Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent &) {
        if (guest.alive() && !captured) {
          video->SetFocus();
          video->CaptureMouse();
          captured = video->HasCapture();
          if (captured) {
            video->SetCursor(wxCursor(wxCURSOR_BLANK));
            recenter();
          }
        }
      });
      video->Bind(wxEVT_MOTION, [this](wxMouseEvent &e) {
        if (captured) {
          wxSize s = video->GetClientSize();
          int dx = e.GetX() - s.x / 2, dy = e.GetY() - s.y / 2;
          if (dx || dy) {
            yaw -= dx * .0025;
            pitch = std::clamp(pitch - dy * .0025, -pi * .49, pi * .49);
            recenter();
          }
        }
      });
      video->Bind(wxEVT_MOUSE_CAPTURE_LOST, [this](wxMouseCaptureLostEvent &) {
        captured = false;
        keys.fill(false);
        video->SetCursor(wxNullCursor);
      });
      video->Bind(wxEVT_KEY_DOWN, [this](wxKeyEvent &e) {
        int key = e.GetKeyCode();
        if (key == WXK_ESCAPE)
          release_mouse();
        else if (key >= 0 && key < (int)keys.size())
          keys[key] = true;
      });
      video->Bind(wxEVT_KEY_UP, [this](wxKeyEvent &e) {
        int key = e.GetKeyCode();
        if (key >= 0 && key < (int)keys.size())
          keys[key] = false;
      });
      video->Bind(wxEVT_KILL_FOCUS, [this](wxFocusEvent &e) {
        release_mouse();
        e.Skip();
      });
    }
    SetSizer(root);
    Bind(wxEVT_TIMER, &Viewer::tick, this);
    Bind(wxEVT_CLOSE_WINDOW, [this](wxCloseEvent &e) {
      timer.Stop();
      release_mouse();
      guest.disconnect();
      admin.disconnect();
      e.Skip();
    });
    timer.Start(16);
  }
};
class ViewerApp : public wxApp {
public:
  bool OnInit() override {
    bool admin_only = false;
    wxString host = "127.0.0.1";
    for (int i = 1; i < argc; ++i) {
      wxString arg = argv[i];
      if (arg == "--admin-only")
        admin_only = true;
      else if (arg == "--host" && i + 1 < argc)
        host = argv[++i];
      else {
        wxFprintf(stderr, "Usage: %s [--host HOST] [--admin-only]\n", argv[0]);
        return false;
      }
    }
    auto *frame = new Viewer(admin_only, host);
    frame->Show();
    return true;
  }
};
wxIMPLEMENT_APP_NO_MAIN(ViewerApp);
int main(int argc, char **argv) {
#if defined(__linux__)
  // Mouse-look uses pointer warping, which GTK cannot provide under Wayland.
  // Prefer X11/XWayland when available unless the user selected a backend.
  if (!std::getenv("GDK_BACKEND") && std::getenv("WAYLAND_DISPLAY") &&
      std::getenv("DISPLAY"))
    setenv("GDK_BACKEND", "x11", 0);
#endif
  return wxEntry(argc, argv);
}
