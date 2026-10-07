#pragma once
// FFmpeg-only backend selection and encoding. No Unreal, MuJoCo or socket access.
// This core is also exercised by the standalone hardware diagnostic.
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
namespace URSoccerLab::Media
{
struct Profile
{
    int Width = 640, Height = 480;
    double Rate = 30;
    int Bitrate = 2000000, Gop = 60;
};
struct Policy
{
    std::string Codec = "av1", Backend = "auto", Fallback = "h264", Device;
    std::vector<Profile> Profiles;
};
struct Backend
{
    std::string Codec, Api, Device;
    bool LowPower = false;
    int DecodedWidth = 0, DecodedHeight = 0; // measured during calibration for this capture profile
};
struct Packet
{
    std::vector<uint8_t> Bytes, Config;
    int64_t Pts = 0;
    int Width = 0, Height = 0;
    bool Key = false;
};
class Codec
{
  public:
    Codec(const Backend &Backend, const Profile &Profile);
    ~Codec();
    Codec(const Codec &) = delete;
    Codec &operator=(const Codec &) = delete;
    // Feed one owned BGRA frame, or a moving calibration pattern when pixels=null.
    void Send(const uint8_t *Pixels, int64_t Pts);
    bool Receive(Packet &Packet);
    void Flush();

  private:
    struct Impl;
    std::unique_ptr<Impl> State;
};
class Selection
{
  public:
    explicit Selection(Policy Policy);
    void AddProfile(const Profile &Profile);
    Backend Get(const Profile &Actual);

  private:
    Policy Settings;
    std::mutex Mutex;
    bool Chosen = false, Failed = false;
    Backend Selected;
    std::string Failure;
    struct ValidatedProfile
    {
        Profile Capture;
        Backend Result;
    };
    std::vector<ValidatedProfile> Checked;
};
} // namespace URSoccerLab::Media
