#include "Scene/URSRobotTypeRegistry.h"

namespace URSoccerLab
{
FURSRobotTypeRegistry& FURSRobotTypeRegistry::Get()
{
	static FURSRobotTypeRegistry Instance;
	return Instance;
}

void FURSRobotTypeRegistry::Register(const FURSRobotType& Type)
{
	if (Type.Name.IsEmpty())
	{
		return;
	}
	Types.Add(Type.Name, Type);
}

void FURSRobotTypeRegistry::RegisterDefaultTypes()
{
	if (bDefaultsRegistered)
	{
		return;
	}
	bDefaultsRegistered = true;

	FURSRobotType PiPlus;
	PiPlus.Name = TEXT("pi_plus");
	PiPlus.BlueprintAssetPath = TEXT("/Game/URSoccerLab/Robots/pi_plus/pi_plus.pi_plus");
	PiPlus.DefaultBaseHeightM = 0.3762;
	PiPlus.DefaultJointPositions = {
		{TEXT("l_hip_pitch_joint"), -0.25f},
		{TEXT("l_hip_roll_joint"), 0.0f},
		{TEXT("l_thigh_joint"), 0.0f},
		{TEXT("l_calf_joint"), 0.65f},
		{TEXT("l_ankle_pitch_joint"), -0.4f},
		{TEXT("l_ankle_roll_joint"), 0.0f},
		{TEXT("r_hip_pitch_joint"), -0.25f},
		{TEXT("r_hip_roll_joint"), 0.0f},
		{TEXT("r_thigh_joint"), 0.0f},
		{TEXT("r_calf_joint"), 0.65f},
		{TEXT("r_ankle_pitch_joint"), -0.4f},
		{TEXT("r_ankle_roll_joint"), 0.0f},
		{TEXT("l_shoulder_pitch_joint"), 0.0f},
		{TEXT("l_shoulder_roll_joint"), 0.2f},
		{TEXT("l_upper_arm_joint"), 0.0f},
		{TEXT("l_elbow_joint"), -1.2f},
		{TEXT("r_shoulder_pitch_joint"), 0.0f},
		{TEXT("r_shoulder_roll_joint"), -0.2f},
		{TEXT("r_upper_arm_joint"), 0.0f},
		{TEXT("r_elbow_joint"), -1.2f},
		{TEXT("head_yaw_joint"), 0.0f},
		{TEXT("head_pitch_joint"), 0.0f},
	};
	Register(PiPlus);
}

const FURSRobotType* FURSRobotTypeRegistry::Find(const FString& Name) const
{
	return Types.Find(Name);
}

TArray<FString> FURSRobotTypeRegistry::GetRegisteredNames() const
{
	TArray<FString> Names;
	Types.GetKeys(Names);
	return Names;
}
} // namespace URSoccerLab
