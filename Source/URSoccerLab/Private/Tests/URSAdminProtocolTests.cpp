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

	TestEqual(TEXT("missing op rejected"),
		FAdminProtocol::ParseRequest(TEXT("{}"), Req),
		EAdminRequestParse::MissingOp);

	TestEqual(TEXT("empty op rejected"),
		FAdminProtocol::ParseRequest(TEXT("{\"op\":\"\"}"), Req),
		EAdminRequestParse::MissingOp);

	TestEqual(TEXT("unknown op rejected"),
		FAdminProtocol::ParseRequest(TEXT("{\"op\":\"teleport\"}"), Req),
		EAdminRequestParse::UnknownOp);

	TestEqual(TEXT("reset op accepted"),
		FAdminProtocol::ParseRequest(TEXT("{\"op\":\"reset\"}"), Req),
		EAdminRequestParse::Accepted);
	TestEqual(TEXT("reset op stored"), Req.Op, EAdminOp::Reset);
	TestFalse(TEXT("reset has no translation"), Req.TranslationMeters.IsSet());

	TestEqual(TEXT("set_pose with no fields accepted"),
		FAdminProtocol::ParseRequest(TEXT("{\"op\":\"set_pose\"}"), Req),
		EAdminRequestParse::Accepted);
	TestEqual(TEXT("set_pose stored"), Req.Op, EAdminOp::SetPose);
	TestFalse(TEXT("no translation default"), Req.TranslationMeters.IsSet());
	TestFalse(TEXT("no rotation default"), Req.RotationQuatXyzw.IsSet());
	TestFalse(TEXT("no joint default"), Req.JointQpos.IsSet());

	TestEqual(TEXT("set_pose with full body accepted"),
		FAdminProtocol::ParseRequest(
			TEXT("{\"op\":\"set_pose\",\"translation_m\":[0.5,0.0,0.3762],")
			TEXT("\"rotation_quat_xyzw\":[0,0,0,1],\"joint_qpos\":[0.1,-0.1]}"), Req),
		EAdminRequestParse::Accepted);
	TestTrue(TEXT("translation set"), Req.TranslationMeters.IsSet());
	TestEqual(TEXT("translation X"), Req.TranslationMeters.GetValue().X, 0.5);
	TestTrue(TEXT("rotation set"), Req.RotationQuatXyzw.IsSet());
	TestEqual(TEXT("rotation W"), Req.RotationQuatXyzw.GetValue().W, 1.0);
	TestTrue(TEXT("joint qpos set"), Req.JointQpos.IsSet());
	TestEqual(TEXT("joint qpos length"), Req.JointQpos.GetValue().Num(), 2);
	TestEqual(TEXT("joint qpos[1]"), Req.JointQpos.GetValue()[1], -0.1f);

	TestEqual(TEXT("bad translation rejected"),
		FAdminProtocol::ParseRequest(
			TEXT("{\"op\":\"set_pose\",\"translation_m\":[0.5,0.0]}"), Req),
		EAdminRequestParse::BadTranslation);

	TestEqual(TEXT("non-finite translation rejected"),
		FAdminProtocol::ParseRequest(
			TEXT("{\"op\":\"set_pose\",\"translation_m\":[1e999,0,0]}"), Req),
		EAdminRequestParse::BadTranslation);

	TestEqual(TEXT("bad rotation rejected"),
		FAdminProtocol::ParseRequest(
			TEXT("{\"op\":\"set_pose\",\"rotation_quat_xyzw\":[0,0,0]}"), Req),
		EAdminRequestParse::BadRotation);

	TestEqual(TEXT("non-finite joint rejected"),
		FAdminProtocol::ParseRequest(
			TEXT("{\"op\":\"set_pose\",\"joint_qpos\":[0.1,1e999]}"), Req),
		EAdminRequestParse::BadJointQpos);
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

	FAdminPoseRequest RoundTrip;
	TestEqual(TEXT("reply is parseable as JSON"),
		FAdminProtocol::ParseRequest(OkSetPose.Replace(TEXT("\"ok\":true"), TEXT("\"op\":\"set_pose\"")), RoundTrip),
		EAdminRequestParse::Accepted);

	const FString Err = FAdminProtocol::BuildErrorReply(TEXT("set_pose"), TEXT("dim_mismatch"), TEXT("length 5"));
	TestTrue(TEXT("error contains ok=false"), Err.Contains(TEXT("\"ok\":false")));
	TestTrue(TEXT("error contains code"), Err.Contains(TEXT("\"error\":\"dim_mismatch\"")));
	TestTrue(TEXT("error contains message"), Err.Contains(TEXT("length 5")));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
