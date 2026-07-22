#pragma once

#include "CoreMinimal.h"

namespace URSoccerLab
{
enum class EAdminOp : uint8
{
	Unknown,
	SetPose,
	GetPose,
	Reset
};

enum class EAdminRequestParse : uint8
{
	Accepted,
	NotJson,
	MissingOp,
	UnknownOp,
	BadTranslation,
	BadRotation,
	BadJointQpos,
	BadJointQposDim
};

struct FAdminPoseRequest
{
	EAdminOp Op = EAdminOp::Unknown;
	TOptional<FVector> TranslationMeters;
	TOptional<FQuat> RotationQuatXyzw;
	TOptional<TArray<float>> JointQpos;
};

class URSOCCERLAB_API FAdminProtocol
{
public:
	static const TCHAR* LexToString(EAdminRequestParse Status);

	static EAdminRequestParse ParseRequest(const FString& JsonBody, FAdminPoseRequest& Out);
	static FString BuildOkReply(const FString& OpName, const FString& ActorId);
	static FString BuildOkSetPoseReply(
		const FString& ActorId,
		const FVector& AppliedTranslationMeters,
		const FQuat& AppliedRotationXyzw,
		const TArray<float>& AppliedJointQpos,
		double SimTimeSec);
	static FString BuildOkGetPoseReply(
		const FString& ActorId,
		const FVector& TranslationMeters,
		const FQuat& RotationXyzw,
		const TArray<float>& JointQpos,
		double SimTimeSec);
	static FString BuildErrorReply(const FString& OpName, const FString& ErrorCode, const FString& Message);
};
} // namespace URSoccerLab
