#pragma once

#include "CoreMinimal.h"

namespace URSoccerLab
{
enum class EAdminOp : uint8
{
	Unknown,
	SetPose,
	GetPose,
	Reset,
	LockPose,
	UnlockPose
};

enum class EAdminRequestParse : uint8
{
	Accepted,
	NotJson,
	MissingCommand,
	UnknownCommand,
	BadTranslation,
	BadRotation,
	BadJointQpos,
	BadJointQposDim
};

// Parsed admin request. The production wire schema is:
//   {"command": "<op>", "args": {"actor_id": "...", <pose fields>}}
// Pose fields are optional; absent ones are left unset so the runtime can
// distinguish "omit" from "default" (the root cause of the set_pose bug).
struct FAdminPoseRequest
{
	EAdminOp Op = EAdminOp::Unknown;
	FString ActorId;
	TOptional<FVector> TranslationMeters;
	TOptional<FQuat> RotationQuatXyzw;
	TOptional<TArray<float>> JointQpos;
};

class URSOCCERLAB_API FAdminProtocol
{
public:
	static const TCHAR* LexToString(EAdminRequestParse Status);
	static FString CommandName(EAdminOp Op);

	// Parse a {"command","args"} admin request. Absent pose fields remain
	// unset on Out, so the caller can forward nullptr for them.
	static EAdminRequestParse ParseRequest(const FString& JsonBody, FAdminPoseRequest& Out);

	static FString BuildOkReply(const FString& CommandName, const FString& ActorId);
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
	static FString BuildErrorReply(const FString& CommandName, const FString& ErrorCode, const FString& Message);
};
} // namespace URSoccerLab
