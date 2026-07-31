#pragma once

#include "CoreMinimal.h"

namespace URSoccerLab::TcpProtocol
{
inline constexpr uint8 TypeJson = 0x00;
inline constexpr uint8 TypeRgb = 0x01;
inline constexpr uint8 TypeDepth = 0x02;

inline constexpr uint8 ImageMessageVersion = 0x02;

inline constexpr uint8 ImageCodecRaw = 0x00;
inline constexpr uint8 ImageCodecJpeg = 0x01;
inline constexpr uint8 ImageCodecZlib = 0x02;

inline constexpr uint8 PixelFormatBgra8 = 0x00;
inline constexpr uint8 PixelFormatDepthFloat32Meters = 0x01;
inline constexpr uint8 PixelFormatDepthUint16Millimeters = 0x02;

inline constexpr int32 MinTcpPort = 1024;
inline constexpr int32 MaxTcpPort = 65535;
inline constexpr int32 DefaultRobotBasePort = 10000;
inline constexpr int32 DefaultAdminPort = 11000;

/** Builds one bidirectional TCP port per robot. */
URSOCCERLAB_API bool BuildRobotPorts(int32 BasePort, int32 RobotCount, TArray<int32>& OutPorts);

/** Validates the robot range and single optional global administration port. */
URSOCCERLAB_API bool IsValidPortLayout(int32 RobotBasePort, int32 RobotCount, int32 AdminPort);

URSOCCERLAB_API bool DoesAdminPortCollide(int32 RobotBasePort, int32 RobotCount, int32 AdminPort);
} // namespace URSoccerLab::TcpProtocol
