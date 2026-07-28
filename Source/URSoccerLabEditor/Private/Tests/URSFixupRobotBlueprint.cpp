#if WITH_DEV_AUTOMATION_TESTS

#include "CoreMinimal.h"
#include "Editor.h"
#include "Engine/Blueprint.h"
#include "Misc/AutomationTest.h"
#include "Misc/PackageName.h"
#include "UObject/SavePackage.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FURSFixupRobotBlueprint,
	"URSoccerLab.Maintenance.FixupRobotBlueprint",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FURSFixupRobotBlueprint::RunTest(const FString& Parameters)
{
	const FString NewPath = TEXT("/Game/URSoccerLab/Robots/pi_plus/pi_plus.pi_plus");

	// Already at new path?
	if (UBlueprint* Existing = LoadObject<UBlueprint>(nullptr, *NewPath))
	{
		UE_LOG(LogTemp, Log, TEXT("[fixup] Already at new path."));
		return true;
	}

	// Load from old path.
	const FString OldPath = TEXT("/Game/MuJoCoImports/pi_plus_stereo_camera.pi_plus_stereo_camera");
	UBlueprint* OldBP = LoadObject<UBlueprint>(nullptr, *OldPath);
	TestNotNull(TEXT("old Blueprint loadable"), OldBP);
	if (!OldBP) return false;

	UE_LOG(LogTemp, Log, TEXT("[fixup] Loaded from %s, renaming..."), *OldPath);

	const FString NewPkgPath = TEXT("/Game/URSoccerLab/Robots/pi_plus/pi_plus");
	UPackage* NewPkg = CreatePackage(*NewPkgPath);

	// Move the Blueprint to the new package using Rename.
	if (!OldBP->Rename(TEXT("pi_plus"), NewPkg, REN_DontCreateRedirectors))
	{
		AddError(TEXT("Rename failed"));
		return false;
	}

	UE_LOG(LogTemp, Log, TEXT("[fixup] Renamed to %s"), *OldBP->GetPathName());

	NewPkg->MarkAsFullyLoaded();
	NewPkg->SetDirtyFlag(true);

	// Save to disk.
	FString Filename = FPackageName::LongPackageNameToFilename(
		NewPkgPath, FPackageName::GetAssetPackageExtension());
	FSavePackageArgs SaveArgs;
	SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
	SaveArgs.SaveFlags = SAVE_NoError;
	bool bSaved = UPackage::SavePackage(NewPkg, OldBP, *Filename, SaveArgs);
	TestTrue(TEXT("saved package"), bSaved);

	if (bSaved)
		UE_LOG(LogTemp, Log, TEXT("[fixup] Saved %s"), *Filename);

	return bSaved;
}

#endif // WITH_DEV_AUTOMATION_TESTS
