#if WITH_DEV_AUTOMATION_TESTS

#include "Runtime/URSAdminProtocol.h"

#include "Misc/AutomationTest.h"

using namespace URSoccerLab;

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FURSAdminRequestParseTest,
	"URSoccerLab.Admin.Protocol.RequestParse",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FURSAdminRequestParseTest::RunTest(const FString& Parameters)
{
	FAdminPoseRequest Req;

	TestEqual(TEXT("not JSON rejected"),
		FAdminProtocol::ParseRequest(TEXT("not json"), Req),
		EAdminRequestParse::NotJson);

	TestEqual(TEXT("missing command rejected"),
		FAdminProtocol::ParseRequest(TEXT("{}"), Req),
		EAdminRequestParse::MissingCommand);

	TestEqual(TEXT("empty command rejected"),
		FAdminProtocol::ParseRequest(TEXT("{\"command\":\"\"}"), Req),
		EAdminRequestParse::MissingCommand);

	TestEqual(TEXT("unknown command rejected"),
		FAdminProtocol::ParseRequest(TEXT("{\"command\":\"teleport\",\"args\":{\"actor_id\":\"rp0\"}}"), Req),
		EAdminRequestParse::UnknownCommand);

	TestEqual(TEXT("reset accepted"),
		FAdminProtocol::ParseRequest(TEXT("{\"command\":\"reset\",\"args\":{\"actor_id\":\"robot_rp0\"}}"), Req),
		EAdminRequestParse::Accepted);
	TestEqual(TEXT("reset op stored"), Req.Op, EAdminOp::Reset);
	TestEqual(TEXT("reset actor_id stored"), Req.ActorId, TEXT("robot_rp0"));
	TestFalse(TEXT("reset has no translation"), Req.TranslationMeters.IsSet());

	TestEqual(TEXT("get_pose accepted"),
		FAdminProtocol::ParseRequest(TEXT("{\"command\":\"get_pose\",\"args\":{\"actor_id\":\"robot_rp0\"}}"), Req),
		EAdminRequestParse::Accepted);
	TestEqual(TEXT("get_pose op stored"), Req.Op, EAdminOp::GetPose);

	TestEqual(TEXT("set_pose with no fields accepted"),
		FAdminProtocol::ParseRequest(TEXT("{\"command\":\"set_pose\",\"args\":{\"actor_id\":\"robot_rp0\"}}"), Req),
		EAdminRequestParse::Accepted);
	TestEqual(TEXT("set_pose op stored"), Req.Op, EAdminOp::SetPose);
	TestFalse(TEXT("no translation default"), Req.TranslationMeters.IsSet());
	TestFalse(TEXT("no rotation default"), Req.RotationQuatXyzw.IsSet());
	TestFalse(TEXT("no joint default"), Req.JointQpos.IsSet());

	TestEqual(TEXT("set_pose with full body accepted"),
		FAdminProtocol::ParseRequest(
			TEXT("{\"command\":\"set_pose\",\"args\":{\"actor_id\":\"robot_rp0\",")
			TEXT("\"translation_m\":[0.5,0.0,0.3762],")
			TEXT("\"rotation_quat_xyzw\":[0,0,0,1],\"joint_qpos\":[0.1,-0.1]}}"), Req),
		EAdminRequestParse::Accepted);
	TestTrue(TEXT("translation set"), Req.TranslationMeters.IsSet());
	TestEqual(TEXT("translation X"), Req.TranslationMeters.GetValue().X, 0.5);
	TestTrue(TEXT("rotation set"), Req.RotationQuatXyzw.IsSet());
	TestEqual(TEXT("rotation W"), Req.RotationQuatXyzw.GetValue().W, 1.0);
	TestTrue(TEXT("joint qpos set"), Req.JointQpos.IsSet());
	TestEqual(TEXT("joint qpos length"), Req.JointQpos.GetValue().Num(), 2);
	TestEqual(TEXT("joint qpos[1]"), Req.JointQpos.GetValue()[1], -0.1f);

	// Optional fields: a translation-only request must leave rotation/joint unset.
	TestEqual(TEXT("translation-only leaves others unset"),
		FAdminProtocol::ParseRequest(
			TEXT("{\"command\":\"set_pose\",\"args\":{\"actor_id\":\"robot_rp0\",")
			TEXT("\"translation_m\":[0.5,0.0,0.3762]}}"), Req),
		EAdminRequestParse::Accepted);
	TestTrue(TEXT("translation-only: translation set"), Req.TranslationMeters.IsSet());
	TestFalse(TEXT("translation-only: rotation unset"), Req.RotationQuatXyzw.IsSet());
	TestFalse(TEXT("translation-only: joint unset"), Req.JointQpos.IsSet());

	TestEqual(TEXT("bad translation rejected"),
		FAdminProtocol::ParseRequest(
			TEXT("{\"command\":\"set_pose\",\"args\":{\"actor_id\":\"rp0\",\"translation_m\":[0.5,0.0]}}"), Req),
		EAdminRequestParse::BadTranslation);

	TestEqual(TEXT("non-finite translation rejected"),
		FAdminProtocol::ParseRequest(
			TEXT("{\"command\":\"set_pose\",\"args\":{\"actor_id\":\"rp0\",\"translation_m\":[1e999,0,0]}}"), Req),
		EAdminRequestParse::BadTranslation);

	TestEqual(TEXT("bad rotation rejected"),
		FAdminProtocol::ParseRequest(
			TEXT("{\"command\":\"set_pose\",\"args\":{\"actor_id\":\"rp0\",\"rotation_quat_xyzw\":[0,0,0]}}"), Req),
		EAdminRequestParse::BadRotation);

	TestEqual(TEXT("non-finite joint rejected"),
		FAdminProtocol::ParseRequest(
			TEXT("{\"command\":\"set_pose\",\"args\":{\"actor_id\":\"rp0\",\"joint_qpos\":[0.1,1e999]}}"), Req),
		EAdminRequestParse::BadJointQpos);

	TestEqual(TEXT("lock_pose accepted"),
		FAdminProtocol::ParseRequest(TEXT("{\"command\":\"lock_pose\",\"args\":{\"actor_id\":\"robot_rp0\"}}"), Req),
		EAdminRequestParse::Accepted);
	TestEqual(TEXT("lock_pose op stored"), Req.Op, EAdminOp::LockPose);

	TestEqual(TEXT("unlock_pose accepted"),
		FAdminProtocol::ParseRequest(TEXT("{\"command\":\"unlock_pose\",\"args\":{\"actor_id\":\"robot_rp0\"}}"), Req),
		EAdminRequestParse::Accepted);
	TestEqual(TEXT("unlock_pose op stored"), Req.Op, EAdminOp::UnlockPose);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FURSAdminReplyBuildTest,
	"URSoccerLab.Admin.Protocol.ReplyBuild",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FURSAdminReplyBuildTest::RunTest(const FString& Parameters)
{
	const FString OkReset = FAdminProtocol::BuildOkReply(TEXT("reset"), TEXT("robot_rp0"));
	TestTrue(TEXT("ok reset contains ok=true"), OkReset.Contains(TEXT("\"ok\":true")));
	TestTrue(TEXT("ok reset contains op"), OkReset.Contains(TEXT("\"op\":\"reset\"")));
	TestTrue(TEXT("ok reset contains actor_id"), OkReset.Contains(TEXT("\"actor_id\":\"robot_rp0\"")));

	const FString OkSetPose = FAdminProtocol::BuildOkSetPoseReply(
		TEXT("robot_rp0"),
		FVector(0.5, 0.0, 0.3762),
		FQuat(0, 0, 0, 1),
		{0.1f, -0.1f},
		1.5);
	TestTrue(TEXT("set_pose reply contains ok=true"), OkSetPose.Contains(TEXT("\"ok\":true")));
	TestTrue(TEXT("set_pose reply contains translation"), OkSetPose.Contains(TEXT("applied_translation_m")));
	TestTrue(TEXT("set_pose reply contains joint_qpos"), OkSetPose.Contains(TEXT("applied_joint_qpos")));
	TestTrue(TEXT("set_pose reply contains sim_time"), OkSetPose.Contains(TEXT("\"sim_time_sec\":1.5")));

	const FString OkGetPose = FAdminProtocol::BuildOkGetPoseReply(
		TEXT("robot_rp0"),
		FVector(0.5, 0.0, 0.3762),
		FQuat(0, 0, 0, 1),
		{0.1f, -0.1f},
		1.5);
	TestTrue(TEXT("get_pose reply contains translation_m"), OkGetPose.Contains(TEXT("\"translation_m\"")));
	TestTrue(TEXT("get_pose reply contains joint_qpos"), OkGetPose.Contains(TEXT("\"joint_qpos\"")));

	const FString Err = FAdminProtocol::BuildErrorReply(TEXT("set_pose"), TEXT("dim_mismatch"), TEXT("length 5"));
	TestTrue(TEXT("error contains ok=false"), Err.Contains(TEXT("\"ok\":false")));
	TestTrue(TEXT("error contains code"), Err.Contains(TEXT("\"error\":\"dim_mismatch\"")));
	TestTrue(TEXT("error contains message"), Err.Contains(TEXT("length 5")));

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
