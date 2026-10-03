#pragma once
#include "CoreMinimal.h"
namespace URSoccerLab::TcpFraming
{
inline constexpr int32 MaxFrameLength = 16 * 1024 * 1024;
inline uint32 ReadLength(const uint8* Data)
{
	return (uint32(Data[0]) << 24) | (uint32(Data[1]) << 16) | (uint32(Data[2]) << 8) | uint32(Data[3]);
}
inline void Append(TArray<uint8>& Buffer, uint8 Type, const uint8* Data, int32 Length)
{
	const uint32 FrameLength = uint32(Length) + 1;
	Buffer.Add(FrameLength >> 24);
	Buffer.Add(FrameLength >> 16);
	Buffer.Add(FrameLength >> 8);
	Buffer.Add(FrameLength);
	Buffer.Add(Type);
	Buffer.Append(Data, Length);
}
} // namespace URSoccerLab::TcpFraming
