// Build against the same FFmpeg libraries as the simulator and URSVideoCodec.cpp.
// Usage: test_encoder_backend CODEC BACKEND FALLBACK OUTPUT_PREFIX [DEVICE]
// FALLBACK="none" disables fallback. Writes length-prefixed packets for receiver tests.
#include "URSVideoCodec.h"
#include <cstdio>
#include <fstream>
#include <stdexcept>
using namespace URSoccerLab::Media;
int main(int argc, char **argv)
{
    try
    {
        if (argc < 5 || argc > 6)
            throw std::runtime_error("Expected CODEC BACKEND FALLBACK OUTPUT_PREFIX [DEVICE]");
        Policy P;
        P.Codec = argv[1];
        P.Backend = argv[2];
        P.Fallback = std::string(argv[3]) == "none" ? "" : argv[3];
        if (argc == 6)
            P.Device = argv[5];
        Profile Guest{640, 480, 30, 2000000, 30}, CustomGuest{800, 600, 30, 2000000, 30}, Stereo{1280, 480, 30, 2000000, 30};
        P.Profiles = {Guest, CustomGuest, Stereo};
        Selection Select(P);
        const Backend B = Select.Get(Guest);
        std::printf("codec=%s backend=%s device=%s\n", B.Codec.c_str(), B.Api.c_str(), B.Device.c_str());
        for (const auto &Profile : P.Profiles)
        {
            const Backend Shared = Select.Get(Profile);
            if (Shared.Codec != B.Codec || Shared.Api != B.Api || Shared.Device != B.Device)
                throw std::runtime_error("Selection changed across profiles");
            Codec Encode(Shared, Profile);
            std::ofstream Out(std::string(argv[4]) + "-" + std::to_string(Profile.Width) + ".pkts", std::ios::binary);
            if (!Out)
                throw std::runtime_error("Cannot open packet output");
            int Count = 0, Keys = 0;
            auto Drain = [&]
            {
                Packet Packet;
                while (Encode.Receive(Packet))
                {
                    // Native-endian diagnostic envelope, Linux x86-64 only.
                    uint32_t Size = Packet.Bytes.size(), Config = Packet.Config.size(), Width = Packet.Width,
                             Height = Packet.Height, Key = Packet.Key;
                    Out.write(reinterpret_cast<char *>(&Size), 4);
                    Out.write(reinterpret_cast<char *>(&Config), 4);
                    Out.write(reinterpret_cast<char *>(&Width), 4);
                    Out.write(reinterpret_cast<char *>(&Height), 4);
                    Out.write(reinterpret_cast<char *>(&Key), 4);
                    Out.write(reinterpret_cast<char *>(&Packet.Pts), 8);
                    Out.write(reinterpret_cast<char *>(Packet.Config.data()), Config);
                    Out.write(reinterpret_cast<char *>(Packet.Bytes.data()), Size);
                    ++Count;
                    Keys += Packet.Key;
                }
            };
            for (int I = 0; I < 90; ++I)
            {
                Encode.Send(nullptr, I);
                Drain();
            }
            Encode.Flush();
            Drain();
            if (Count != 90 || Keys < 3)
                throw std::runtime_error("Missing encoded frames or periodic keyframes");
            std::printf("width=%d packets=%d keyframes=%d\n", Profile.Width, Count, Keys);
        }
        return 0;
    }
    catch (const std::exception &E)
    {
        std::fprintf(stderr, "%s\n", E.what());
        return 1;
    }
}
