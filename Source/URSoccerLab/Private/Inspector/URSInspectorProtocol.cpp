#include "Inspector/URSInspectorProtocol.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Dom/JsonObject.h"
bool URSoccerLab::FInspectorProtocol::Decode(const FString& Json, FInspectorPose& Pose, FString& Error)
{
 Error = TEXT("expected v1 set_camera with finite position and nonzero xyzw quaternion");
 TSharedPtr<FJsonObject> Root;
 if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Root) || !Root) return false;
 double Version = 0; FString Command;
 const TSharedPtr<FJsonObject>* Args;
 if (!Root->HasTypedField<EJson::Number>(TEXT("version")) || !Root->TryGetNumberField(TEXT("version"), Version) || Version != 1 ||
     !Root->TryGetStringField(TEXT("command"), Command) || Command != TEXT("set_camera") ||
     !Root->TryGetObjectField(TEXT("args"), Args)) return false;
 const TArray<TSharedPtr<FJsonValue>> *P, *Q;
 if (!(*Args)->TryGetArrayField(TEXT("translation_m"), P) || P->Num() != 3 ||
     !(*Args)->TryGetArrayField(TEXT("rotation_quat_xyzw"), Q) || Q->Num() != 4) return false;
 double Values[7];
 for (int i = 0; i < 7; ++i)
 {
  const auto& V = i < 3 ? (*P)[i] : (*Q)[i-3];
  if (V->Type != EJson::Number || !V->TryGetNumber(Values[i]) || !FMath::IsFinite(Values[i])) return false;
 }
 for (int i = 0; i < 3; ++i) if (FMath::Abs(Values[i]) > 100)
 { Error = TEXT("camera position must be within +/-100 metres on each axis"); return false; }
 // Rescale before normalization to avoid overflow for finite large inputs.
 double Scale = 0;
 for (int i = 3; i < 7; ++i) Scale = FMath::Max(Scale, FMath::Abs(Values[i]));
 if (Scale < 1e-12) return false;
 Pose.Position = FVector(Values[0], Values[1], Values[2]);
 Pose.Rotation = FQuat(Values[3]/Scale, Values[4]/Scale, Values[5]/Scale, Values[6]/Scale);
 Pose.Rotation.Normalize(); Error.Empty(); return true;
}
FString URSoccerLab::FInspectorProtocol::Reply(bool bOk, const FString& Error)
{
 auto Root = MakeShared<FJsonObject>();
 Root->SetNumberField(TEXT("version"), 1); Root->SetBoolField(TEXT("ok"), bOk);
 Root->SetStringField(TEXT("command"), TEXT("set_camera"));
 if (!bOk) Root->SetStringField(TEXT("error"), Error);
 FString Result; FJsonSerializer::Serialize(Root, TJsonWriterFactory<>::Create(&Result)); return Result;
}
