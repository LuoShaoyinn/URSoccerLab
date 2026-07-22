#include "Runtime/URSAdminProtocol.h"

#include "Dom/JsonObject.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace URSoccerLab
{
namespace
{
bool IsFiniteVec(const FVector& V)
{
	return FMath::IsFinite(V.X) && FMath::IsFinite(V.Y) && FMath::IsFinite(V.Z);
}

bool IsFiniteQuat(const FQuat& Q)
{
	return FMath::IsFinite(Q.X) && FMath::IsFinite(Q.Y)
		&& FMath::IsFinite(Q.Z) && FMath::IsFinite(Q.W);
}

bool ReadVec3(const TArray<TSharedPtr<FJsonValue>>* Arr, FVector& Out)
{
	if (!Arr || Arr->Num() != 3)
	{
		return false;
	}
	Out.X = static_cast<double>((*Arr)[0]->AsNumber());
	Out.Y = static_cast<double>((*Arr)[1]->AsNumber());
	Out.Z = static_cast<double>((*Arr)[2]->AsNumber());
	return IsFiniteVec(Out);
}

bool ReadQuatXyzw(const TArray<TSharedPtr<FJsonValue>>* Arr, FQuat& Out)
{
	if (!Arr || Arr->Num() != 4)
	{
		return false;
	}
	Out.X = static_cast<double>((*Arr)[0]->AsNumber());
	Out.Y = static_cast<double>((*Arr)[1]->AsNumber());
	Out.Z = static_cast<double>((*Arr)[2]->AsNumber());
	Out.W = static_cast<double>((*Arr)[3]->AsNumber());
	return IsFiniteQuat(Out);
}

bool ReadFloatArray(const TArray<TSharedPtr<FJsonValue>>* Arr, TArray<float>& Out)
{
	if (!Arr)
	{
		return false;
	}
	Out.Reset(Arr->Num());
	for (const TSharedPtr<FJsonValue>& Val : *Arr)
	{
		const double Num = Val->AsNumber();
		if (!FMath::IsFinite(Num))
		{
			return false;
		}
		Out.Add(static_cast<float>(Num));
	}
	return true;
}

TArray<TSharedPtr<FJsonValue>> NumberArray(const TArray<float>& Values)
{
	TArray<TSharedPtr<FJsonValue>> Out;
	Out.Reserve(Values.Num());
	for (float V : Values)
	{
		Out.Add(MakeShared<FJsonValueNumber>(V));
	}
	return Out;
}

TArray<TSharedPtr<FJsonValue>> Vec3Array(const FVector& V)
{
	return {
		MakeShared<FJsonValueNumber>(V.X),
		MakeShared<FJsonValueNumber>(V.Y),
		MakeShared<FJsonValueNumber>(V.Z)
	};
}

TArray<TSharedPtr<FJsonValue>> QuatArray(const FQuat& Q)
{
	return {
		MakeShared<FJsonValueNumber>(Q.X),
		MakeShared<FJsonValueNumber>(Q.Y),
		MakeShared<FJsonValueNumber>(Q.Z),
		MakeShared<FJsonValueNumber>(Q.W)
	};
}
} // namespace

const TCHAR* FAdminProtocol::LexToString(EAdminRequestParse Status)
{
	switch (Status)
	{
	case EAdminRequestParse::Accepted:
		return TEXT("Accepted");
	case EAdminRequestParse::NotJson:
		return TEXT("NotJson");
	case EAdminRequestParse::MissingOp:
		return TEXT("MissingOp");
	case EAdminRequestParse::UnknownOp:
		return TEXT("UnknownOp");
	case EAdminRequestParse::BadTranslation:
		return TEXT("BadTranslation");
	case EAdminRequestParse::BadRotation:
		return TEXT("BadRotation");
	case EAdminRequestParse::BadJointQpos:
		return TEXT("BadJointQpos");
	case EAdminRequestParse::BadJointQposDim:
		return TEXT("BadJointQposDim");
	}
	return TEXT("Unknown");
}

EAdminRequestParse FAdminProtocol::ParseRequest(const FString& JsonBody, FAdminPoseRequest& Out)
{
	Out = FAdminPoseRequest();

	TSharedPtr<FJsonObject> Root;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonBody);
	if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
	{
		return EAdminRequestParse::NotJson;
	}

	FString OpStr;
	if (!Root->TryGetStringField(TEXT("op"), OpStr) || OpStr.IsEmpty())
	{
		return EAdminRequestParse::MissingOp;
	}
	if (OpStr == TEXT("set_pose"))
	{
		Out.Op = EAdminOp::SetPose;
	}
	else if (OpStr == TEXT("reset"))
	{
		Out.Op = EAdminOp::Reset;
		return EAdminRequestParse::Accepted;
	}
	else
	{
		return EAdminRequestParse::UnknownOp;
	}

	const TArray<TSharedPtr<FJsonValue>>* TransArr = nullptr;
	if (Root->TryGetArrayField(TEXT("translation_m"), TransArr))
	{
		FVector Trans;
		if (!ReadVec3(TransArr, Trans))
		{
			return EAdminRequestParse::BadTranslation;
		}
		Out.TranslationMeters = Trans;
	}

	const TArray<TSharedPtr<FJsonValue>>* RotArr = nullptr;
	if (Root->TryGetArrayField(TEXT("rotation_quat_xyzw"), RotArr))
	{
		FQuat Rot;
		if (!ReadQuatXyzw(RotArr, Rot))
		{
			return EAdminRequestParse::BadRotation;
		}
		Out.RotationQuatXyzw = Rot;
	}

	const TArray<TSharedPtr<FJsonValue>>* JointArr = nullptr;
	if (Root->TryGetArrayField(TEXT("joint_qpos"), JointArr))
	{
		TArray<float> Qpos;
		if (!ReadFloatArray(JointArr, Qpos))
		{
			return EAdminRequestParse::BadJointQpos;
		}
		Out.JointQpos = MoveTemp(Qpos);
	}
	return EAdminRequestParse::Accepted;
}

FString FAdminProtocol::BuildOkReply(const FString& OpName, const FString& ActorId)
{
	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("ok"), TEXT("true"));
	Root->SetStringField(TEXT("op"), OpName);
	Root->SetStringField(TEXT("actor_id"), ActorId);

	FString Json;
	TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
		TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Json);
	FJsonSerializer::Serialize(Root.ToSharedRef(), Writer);
	return Json;
}

FString FAdminProtocol::BuildOkSetPoseReply(
	const FString& ActorId,
	const FVector& AppliedTranslationMeters,
	const FQuat& AppliedRotationXyzw,
	const TArray<float>& AppliedJointQpos,
	double SimTimeSec)
{
	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("ok"), TEXT("true"));
	Root->SetStringField(TEXT("op"), TEXT("set_pose"));
	Root->SetStringField(TEXT("actor_id"), ActorId);
	Root->SetArrayField(TEXT("applied_translation_m"), Vec3Array(AppliedTranslationMeters));
	Root->SetArrayField(TEXT("applied_rotation_quat_xyzw"), QuatArray(AppliedRotationXyzw));
	Root->SetArrayField(TEXT("applied_joint_qpos"), NumberArray(AppliedJointQpos));
	Root->SetNumberField(TEXT("sim_time_sec"), SimTimeSec);

	FString Json;
	TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
		TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Json);
	FJsonSerializer::Serialize(Root.ToSharedRef(), Writer);
	return Json;
}

FString FAdminProtocol::BuildErrorReply(const FString& OpName, const FString& ErrorCode, const FString& Message)
{
	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("ok"), TEXT("false"));
	Root->SetStringField(TEXT("op"), OpName);
	Root->SetStringField(TEXT("error"), ErrorCode);
	Root->SetStringField(TEXT("message"), Message);

	FString Json;
	TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
		TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Json);
	FJsonSerializer::Serialize(Root.ToSharedRef(), Writer);
	return Json;
}
} // namespace URSoccerLab
