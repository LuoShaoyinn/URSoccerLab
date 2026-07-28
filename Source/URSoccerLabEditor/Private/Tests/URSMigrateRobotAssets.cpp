#if WITH_DEV_AUTOMATION_TESTS

#include "CoreMinimal.h"
#include "Editor.h"
#include "Engine/Blueprint.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetToolsModule.h"
#include "IAssetTools.h"
#include "Misc/AutomationTest.h"
#include "Misc/PackageName.h"
#include "UObject/SavePackage.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FURSMigrateRobotAssets,
	"URSoccerLab.Maintenance.MigrateRobotAssets",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FURSMigrateRobotAssets::RunTest(const FString& Parameters)
{
	FAssetRegistryModule& ARM = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
	IAssetRegistry& Registry = ARM.Get();

	TArray<FAssetData> OldAssets;
	Registry.GetAssetsByPath(FName("/Game/MuJoCoImports"), OldAssets, true);

	TArray<FAssetData> ToMigrate;
	for (const FAssetData& Asset : OldAssets)
	{
		FString PkgPath = Asset.PackagePath.ToString();
		if (PkgPath.StartsWith("/Game/MuJoCoImports/pi_plus_stereo_camera"))
		{
			ToMigrate.Add(Asset);
		}
	}

	if (ToMigrate.Num() == 0)
	{
		UE_LOG(LogTemp, Warning, TEXT("[migrate] No pi_plus_stereo_camera assets found under MuJoCoImports. Already migrated?"));
		return true;
	}

	UE_LOG(LogTemp, Log, TEXT("[migrate] Found %d assets to migrate."), ToMigrate.Num());

	FAssetToolsModule& AssetToolsModule = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools");
	IAssetTools& AssetTools = AssetToolsModule.Get();

	TArray<FAssetRenameData> RenameData;
	TMap<UObject*, FAssetRenameData> AssetToRename;

	for (const FAssetData& Asset : ToMigrate)
	{
		UObject* LoadedAsset = Asset.GetAsset();
		if (!LoadedAsset)
		{
			UE_LOG(LogTemp, Warning, TEXT("[migrate] Could not load %s, skipping."), *Asset.GetObjectPathString());
			continue;
		}

		FString OldPkgPath = Asset.PackagePath.ToString();
		FString NewPkgPath;
		FString NewName = Asset.AssetName.ToString();

		if (Asset.AssetName.ToString() == "pi_plus_stereo_camera")
		{
			NewPkgPath = "/Game/URSoccerLab/Robots/pi_plus";
			NewName = "pi_plus";
		}
		else
		{
			NewPkgPath = OldPkgPath.Replace(
				TEXT("/Game/MuJoCoImports/pi_plus_stereo_camera_ue_Assets"),
				TEXT("/Game/URSoccerLab/Robots/pi_plus/dependencies"));
		}

		FAssetRenameData& RD = AssetToRename.Add(LoadedAsset);
		RD.Asset = LoadedAsset;
		RD.NewPackagePath = NewPkgPath;
		RD.NewName = NewName;
		UE_LOG(LogTemp, Log, TEXT("[migrate] %s -> %s/%s"),
			*Asset.GetObjectPathString(), *NewPkgPath, *NewName);
	}

	if (AssetToRename.Num() == 0)
	{
		UE_LOG(LogTemp, Error, TEXT("[migrate] No assets could be loaded for migration."));
		return false;
	}

	AssetToRename.GenerateValueArray(RenameData);

	bool bRenameOk = AssetTools.RenameAssets(RenameData);
	TestTrue(TEXT("RenameAssets succeeded"), bRenameOk);
	if (!bRenameOk)
	{
		return false;
	}

	for (auto& Pair : AssetToRename)
	{
		UObject* AssetObj = Pair.Key;
		if (AssetObj)
		{
			UPackage* Pkg = AssetObj->GetOutermost();
			if (Pkg)
			{
				FString PkgFilename = FPackageName::LongPackageNameToFilename(
					Pkg->GetName(), FPackageName::GetAssetPackageExtension());
				FSavePackageArgs SaveArgs;
				SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
				SaveArgs.SaveFlags = SAVE_NoError;
				UPackage::SavePackage(Pkg, AssetObj, *PkgFilename, SaveArgs);
			}
		}
	}

	UE_LOG(LogTemp, Log, TEXT("[migrate] Migrated %d asset(s)."), AssetToRename.Num());
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
