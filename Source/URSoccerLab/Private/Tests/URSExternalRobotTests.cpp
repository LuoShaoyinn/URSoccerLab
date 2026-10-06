#if WITH_DEV_AUTOMATION_TESTS
#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Scene/URSExternalRobot.h"
#include "Scene/URSSceneConfig.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FURSExternalRobotTest, "URSoccerLab.Scene.ExternalRobot.PackageValidation",
                                 EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FURSExternalRobotTest::RunTest(const FString &Parameters)
{
	const FString Directory = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Tests/ExternalRobotFixture"));
	IFileManager::Get().MakeDirectory(*Directory, true);
	const FString Manifest = FPaths::Combine(Directory, TEXT("robot.json"));
	FFileHelper::SaveStringToFile(
	    TEXT(
	        R"({"version":"urs_robot_v1","id":"fixture","model":"model.xml","default_base_height_m":0.5,"bindings":{"base_body":"base","head_body":"base","left_camera":"left","right_camera":"right"}})"),
	    *Manifest);
	// GLB selection validates the path; runtime visual loading is tested separately.
	FFileHelper::SaveStringToFile(TEXT("fixture"), *FPaths::Combine(Directory, TEXT("visual.glb")));
	const FString Model = TEXT(
	    R"(<mujoco><compiler angle="radian"/><asset><mesh name="look" file="visual.glb" scale="2 3 4"/></asset><default><default class="appearance"><geom type="mesh" mesh="look" contype="0" conaffinity="0" mass="0"/></default></default><worldbody><body name="base"><freejoint name="root"/><inertial pos="0 0 0" mass="2" diaginertia="0.1 0.1 0.1"/><geom name="collision" type="sphere" size="0.1"/><frame pos="0.1 0.2 0.3"><geom name="look_geom" class="appearance" pos="0.4 0 0"/></frame><camera name="left" resolution="640 480"/><camera name="right" pos="0 0.1 0" resolution="640 480"/></body></worldbody></mujoco>)");
	const FString Xml = FPaths::Combine(Directory, TEXT("model.xml"));
	auto Load = [&](const FString &Text, FString &Error) {
		FFileHelper::SaveStringToFile(Text, *Xml);
		return URSoccerLab::FExternalRobotLoader::Load(Manifest, Error);
	};
	FString Error;
	auto Package = Load(Model, Error);
	TestTrue(TEXT("external model loads"), Package.IsValid());
	if (Package)
	{
		TestEqual(TEXT("one GLB visual"), Package->Visuals.Num(), 1);
		TestEqual(TEXT("only MJCF collision geom remains"), Package->Model->ngeom, 1);
		TestEqual(TEXT("explicit inertia mass preserved"), Package->Model->body_mass[1], 2.0);
		TestTrue(TEXT("GLB absent from physics XML"), !Package->PhysicsXml.Contains(TEXT(".glb")));
		TestEqual(TEXT("mesh scale preserved"), Package->Visuals[0].Scale, FVector(2, 3, 4));
		int Site = mj_name2id(Package->Model, mjOBJ_SITE, TCHAR_TO_UTF8(*Package->Visuals[0].SiteName));
		TestTrue(TEXT("nested frame visual offset resolved"),
		         Site >= 0 && FMath::IsNearlyEqual(Package->Model->site_pos[Site * 3], .5, 1e-8) &&
		             FMath::IsNearlyEqual(Package->Model->site_pos[Site * 3 + 1], .2, 1e-8));
	}
	const FString FrameClassModel =
	    Model
	        .Replace(TEXT("<frame pos=\"0.1 0.2 0.3\">"),
			         TEXT("<frame childclass=\"appearance\" pos=\"0.1 0.2 0.3\">"))
	        .Replace(TEXT("class=\"appearance\" pos=\"0.4 0 0\""), TEXT("pos=\"0.4 0 0\""));
	auto FrameClass = Load(FrameClassModel, Error);
	TestTrue(TEXT("frame childclass selects GLB visual"), FrameClass && FrameClass->Visuals.Num() == 1);
	const FString OrientedModel =
	    Model.Replace(TEXT("mass=\"0\"/>"), TEXT("mass=\"0\" quat=\"1 0 0 0\"/>"))
	        .Replace(TEXT("name=\"look_geom\""), TEXT("name=\"look_geom\" euler=\"0 0 1.5707963267948966\""));
	auto Oriented = Load(OrientedModel, Error);
	TestTrue(TEXT("explicit orientation overrides default quaternion"), Oriented.IsValid());
	if (Oriented)
	{
		const int Site =
		    mj_name2id(Oriented->Model, mjOBJ_SITE, TCHAR_TO_UTF8(*Oriented->Visuals[0].SiteName));
		TestTrue(TEXT("explicit visual yaw resolved"),
		         Site >= 0 &&
		             FMath::IsNearlyEqual(Oriented->Model->site_quat[Site * 4], FMath::Sqrt(.5), 1e-8) &&
		             FMath::IsNearlyEqual(Oriented->Model->site_quat[Site * 4 + 3], FMath::Sqrt(.5), 1e-8));
	}
	auto SiteDefaults =
	    Load(Model.Replace(TEXT("<default><default"), TEXT("<default><site pos=\"9 8 7\"/><default")), Error);
	TestTrue(TEXT("site defaults do not alter visual pose"), SiteDefaults.IsValid());
	if (SiteDefaults)
	{
		const int Site =
		    mj_name2id(SiteDefaults->Model, mjOBJ_SITE, TCHAR_TO_UTF8(*SiteDefaults->Visuals[0].SiteName));
		TestTrue(TEXT("visual pose stays in owning frame"),
		         Site >= 0 && FMath::IsNearlyEqual(SiteDefaults->Model->site_pos[Site * 3], .5, 1e-8));
	}
	const FString ClassOrientationModel =
	    Model.Replace(TEXT("<default><default"), TEXT("<default><geom quat=\"1 0 0 0\"/><default"))
	        .Replace(TEXT("<geom type=\"mesh\""),
			         TEXT("<geom euler=\"0 0 1.5707963267948966\" type=\"mesh\""));
	auto ClassOrientation = Load(ClassOrientationModel, Error);
	TestTrue(TEXT("child geom defaults override parent orientation"), ClassOrientation.IsValid());
	const FString PhysicalSiteDefaults = Model.Replace(
	    TEXT("<default><default"), TEXT("<default><site type=\"capsule\" pos=\"9 8 7\" euler=\"0 0 0.5\" "
		                                "fromto=\"0 0 0 0 0 1\" size=\"0.1\"/><default"));
	auto NeutralVisual = Load(PhysicalSiteDefaults, Error);
	TestTrue(TEXT("physical site pose defaults do not leak into visuals"), NeutralVisual.IsValid());
	if (NeutralVisual)
	{
		const int Site =
		    mj_name2id(NeutralVisual->Model, mjOBJ_SITE, TCHAR_TO_UTF8(*NeutralVisual->Visuals[0].SiteName));
		TestTrue(TEXT("visual keeps its geom pose and identity orientation"),
		         Site >= 0 && FMath::IsNearlyEqual(NeutralVisual->Model->site_pos[Site * 3], .5, 1e-8) &&
		             FMath::IsNearlyEqual(NeutralVisual->Model->site_quat[Site * 4], 1.0, 1e-8));
	}
	auto Invalid = Load(Model.Replace(TEXT("contype=\"0\""), TEXT("contype=\"1\"")), Error);
	TestFalse(TEXT("GLB collision rejected"), Invalid.IsValid());
	TestTrue(TEXT("collision error actionable"), Error.Contains(TEXT("define collision separately")));
	Invalid = Load(Model.Replace(TEXT("mass=\"0\""), TEXT("mass=\"1\"")), Error);
	TestFalse(TEXT("GLB mass rejected"), Invalid.IsValid());
	Invalid = Load(
	    Model.Replace(TEXT("<inertial pos=\"0 0 0\" mass=\"2\" diaginertia=\"0.1 0.1 0.1\"/>"), TEXT("")),
	    Error);
	TestFalse(TEXT("missing moving-body inertia rejected"), Invalid.IsValid());
	TestTrue(TEXT("inertia error actionable"), Error.Contains(TEXT("requires explicit <inertial>")));
	Invalid = Load(Model.Replace(TEXT("angle=\"radian\""), TEXT("angle=\"radian\" inertiafromgeom=\"true\"")),
	               Error);
	TestFalse(TEXT("visual-derived inertia rejected"), Invalid.IsValid());
	auto WithoutCollision =
	    Load(Model.Replace(TEXT("<geom name=\"collision\" type=\"sphere\" size=\"0.1\"/>"), TEXT("")), Error);
	TestTrue(TEXT("collision-free model loads with warning"),
	         WithoutCollision &&
	             WithoutCollision->Warnings.Contains(TEXT("robot MJCF contains no collision-enabled geoms")));
	const int32 WorldStart = Model.Find(TEXT("<worldbody>"));
	const int32 WorldEnd = Model.Find(TEXT("</worldbody>")) + FString(TEXT("</worldbody>")).Len();
	const FString IncludeFile = FPaths::Combine(Directory, TEXT("body.xml"));
	FFileHelper::SaveStringToFile(TEXT("<mujocoinclude>") + Model.Mid(WorldStart, WorldEnd - WorldStart) +
	                                  TEXT("</mujocoinclude>"),
	                              *IncludeFile);
	const FString IncludedModel =
	    Model.Left(WorldStart) + TEXT("<include file=\"body.xml\"/>") + Model.Mid(WorldEnd);
	auto Included = Load(IncludedModel, Error);
	TestTrue(TEXT("included body and visual load"),
	         Included && Included->Visuals.Num() == 1 && Included->Model->ngeom == 1);
	FFileHelper::SaveStringToFile(TEXT("<mujocoinclude><include file=\"body.xml\"/></mujocoinclude>"),
	                              *IncludeFile);
	Invalid = Load(IncludedModel, Error);
	TestFalse(TEXT("cyclic includes rejected"), Invalid.IsValid());
	TestTrue(TEXT("include cycle error actionable"), Error.Contains(TEXT("recursive MJCF include")));
	Invalid = Load(Model.Replace(TEXT("name=\"collision\""), TEXT("name=\"__urs_visual_collision\"")), Error);
	TestFalse(TEXT("reserved loader names rejected"), Invalid.IsValid());
	Invalid = Load(Model.Replace(TEXT("640 480"), TEXT("800 600")), Error);
	TestFalse(TEXT("unsupported robot atlas resolution rejected"), Invalid.IsValid());
	IFileManager::Get().Delete(*FPaths::Combine(Directory, TEXT("visual.glb")));
	Invalid = Load(Model, Error);
	TestFalse(TEXT("missing GLB rejected"), Invalid.IsValid());
	IFileManager::Get().DeleteDirectory(*Directory, false, true);
	return true;
}
#endif
