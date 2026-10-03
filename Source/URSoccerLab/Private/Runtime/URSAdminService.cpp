#include "Runtime/URSAdminService.h"
#include "Runtime/URSAdminProtocol.h"
#include "Core/URSRobotCoreComponent.h"
FString URSoccerLab::FAdminService::Execute(UURSRobotCoreComponent* Core, const FString& JsonStr)
{
	FString Reply;
	auto SendReply = [&](const FString& Value) { Reply = Value; };

	using namespace URSoccerLab;

	FAdminPoseRequest Req;
	const EAdminRequestParse Parse = FAdminProtocol::ParseRequest(JsonStr, Req);
	if (Parse != EAdminRequestParse::Accepted)
	{
		SendReply(FAdminProtocol::BuildErrorReply(FAdminProtocol::CommandName(Req.Op),
		                                          FAdminProtocol::LexToString(Parse), TEXT("")));
		return Reply;
	}

	const FString CmdName = FAdminProtocol::CommandName(Req.Op);

	if (!Core)
	{
		SendReply(FAdminProtocol::BuildErrorReply(CmdName, TEXT("not_ready"), TEXT("Core unavailable")));
		return Reply;
	}

	const FVector* Trans = Req.TranslationMeters.IsSet() ? &Req.TranslationMeters.GetValue() : nullptr;
	const FQuat* Rot = Req.RotationQuatXyzw.IsSet() ? &Req.RotationQuatXyzw.GetValue() : nullptr;
	const TArray<float>* Jq = Req.JointQpos.IsSet() ? &Req.JointQpos.GetValue() : nullptr;

	switch (Req.Op)
	{
	case EAdminOp::SetPose: {
		FURSPoseResult R = Core->SetPose(Req.ActorId, Trans, Rot, Jq);
		if (R.bOk)
			SendReply(FAdminProtocol::BuildOkSetPoseReply(Req.ActorId, R.AppliedTranslation, R.AppliedRotation,
			                                              R.AppliedJointQpos, R.SimTime));
		else
			SendReply(FAdminProtocol::BuildErrorReply(CmdName, R.Error, R.Message));
		break;
	}
	case EAdminOp::GetPose: {
		FURSPoseResult R = Core->GetPose(Req.ActorId);
		if (R.bOk)
			SendReply(FAdminProtocol::BuildOkGetPoseReply(Req.ActorId, R.AppliedTranslation, R.AppliedRotation,
			                                              R.AppliedJointQpos, R.SimTime));
		else
			SendReply(FAdminProtocol::BuildErrorReply(CmdName, R.Error, R.Message));
		break;
	}
	case EAdminOp::Reset: {
		FURSPoseResult R = Core->ResetRobot(Req.ActorId);
		if (R.bOk)
			SendReply(FAdminProtocol::BuildOkReply(CmdName, Req.ActorId));
		else
			SendReply(FAdminProtocol::BuildErrorReply(CmdName, R.Error, R.Message));
		break;
	}
	case EAdminOp::LockPose: {
		FURSPoseResult R = Core->SetPoseLock(Req.ActorId, true, Trans, Rot, Jq);
		if (R.bOk)
			SendReply(FAdminProtocol::BuildOkReply(CmdName, Req.ActorId));
		else
			SendReply(FAdminProtocol::BuildErrorReply(CmdName, R.Error, R.Message));
		break;
	}
	case EAdminOp::UnlockPose: {
		FURSPoseResult R = Core->SetPoseLock(Req.ActorId, false);
		if (R.bOk)
			SendReply(FAdminProtocol::BuildOkReply(CmdName, Req.ActorId));
		else
			SendReply(FAdminProtocol::BuildErrorReply(CmdName, R.Error, R.Message));
		break;
	}
	default:
		SendReply(FAdminProtocol::BuildErrorReply(CmdName, TEXT("unknown_command"), TEXT("")));
		break;
	}
	return Reply;
}
