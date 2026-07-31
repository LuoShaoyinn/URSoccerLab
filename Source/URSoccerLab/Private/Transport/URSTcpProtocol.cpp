#include "Transport/URSTcpProtocol.h"

namespace URSoccerLab::TcpProtocol
{
bool BuildRobotPorts(int32 BasePort, int32 RobotCount, TArray<int32>& OutPorts)
{
	OutPorts.Reset();
	if (BasePort < MinTcpPort || RobotCount < 0)
	{
		return false;
	}
	if (RobotCount > 0 && BasePort > MaxTcpPort - (RobotCount - 1))
	{
		return false;
	}

	OutPorts.Reserve(RobotCount);
	for (int32 Index = 0; Index < RobotCount; ++Index)
	{
		OutPorts.Add(BasePort + Index);
	}
	return true;
}

bool DoesAdminPortCollide(int32 RobotBasePort, int32 RobotCount, int32 AdminPort)
{
	return RobotCount > 0
		&& AdminPort >= RobotBasePort
		&& AdminPort <= RobotBasePort + RobotCount - 1;
}

bool IsValidPortLayout(int32 RobotBasePort, int32 RobotCount, int32 AdminPort)
{
	TArray<int32> RobotPorts;
	return BuildRobotPorts(RobotBasePort, RobotCount, RobotPorts)
		&& AdminPort >= MinTcpPort
		&& AdminPort <= MaxTcpPort
		&& !DoesAdminPortCollide(RobotBasePort, RobotCount, AdminPort);
}
} // namespace URSoccerLab::TcpProtocol
