#include "Protocol/URSMessageProtocol.h"
#include "Protocol/URSJson.h"
#include "HAL/PlatformTime.h"
#include <yyjson.h>
namespace URSoccerLab
{
bool FMessageProtocol::DecodeRobotMessage(const TArray<FString>& ActuatorNames, const uint8* Data, int32 Len,
                                          FRobotMessage& Out)
{
	Out = FRobotMessage{};
	if (URSJsonParser::IsControllerParams(Data, Len))
	{
		Out.bControllerParams = true;
		return URSJsonParser::ParseGainParams(Data, Len, Out.Gains, ActuatorNames);
	}
	FCommandSet Cmd;
	bool bAnyActuatorChanged = false;

	yyjson_doc* Doc = yyjson_read((const char*)Data, Len, YYJSON_READ_NOFLAG);
	if (!Doc)
		return false;
	yyjson_val* Root = yyjson_doc_get_root(Doc);
	if (!Root || !yyjson_is_obj(Root))
	{
		yyjson_doc_free(Doc);
		return false;
	}

	const int32 N = FMath::Min(ActuatorNames.Num(), URS_MAX_ACTUATORS);
	yyjson_obj_iter Iter;
	yyjson_obj_iter_init(Root, &Iter);
	yyjson_val* KeyVal;
	while ((KeyVal = yyjson_obj_iter_next(&Iter)))
	{
		yyjson_val* V = yyjson_obj_iter_get_val(KeyVal);
		if (!yyjson_is_num(V))
			continue;
		const char* K = yyjson_get_str(KeyVal);
		if (!K)
			continue;
		FString FName(UTF8_TO_TCHAR(K));
		for (int32 i = 0; i < N; ++i)
		{
			if (ActuatorNames[i] == FName)
			{
				const double Value = yyjson_get_num(V);
				if (FMath::IsFinite(Value))
				{
					const float Target = static_cast<float>(Value);
					if (FMath::IsFinite(Target))
					{
						Cmd.Targets[i] = Target;
						bAnyActuatorChanged = true;
					}
				}
				break;
			}
		}
	}
	yyjson_doc_free(Doc);

	if (!bAnyActuatorChanged)
		return false;
	Cmd.TimestampSec = FPlatformTime::Seconds();
	Cmd.bValid = true;
	Out.Command = Cmd;
	return true;
}
TArray<uint8> FMessageProtocol::EncodeState(const FRobotSnapshot& State, const FRobotMetadata& Meta)
{
	return URSJsonBuilder::BuildStateJson(State, Meta);
}
TArray<uint8> FMessageProtocol::EncodeCamera(const FEncodedCameraFrame& Frame)
{
	TArray<uint8> Payload;
	if (Frame.Images.Num() > 255)
		return Payload;
	Payload.Add(2);
	Payload.Add(Frame.Images.Num());
	const uint16 Flags = 0;
	Payload.Append(reinterpret_cast<const uint8*>(&Flags), 2);
	Payload.Append(reinterpret_cast<const uint8*>(&Frame.Sequence), 4);
	Payload.Append(reinterpret_cast<const uint8*>(&Frame.SimTime), 8);
	for (const auto& Image : Frame.Images)
	{
		FTCHARToUTF8 Name(*Image.Name);
		if (Name.Length() > 255)
			return {};
		Payload.Add(Name.Length());
		Payload.Append(reinterpret_cast<const uint8*>(Name.Get()), Name.Length());
		Payload.Add(Image.Codec);
		Payload.Add(Image.PixelFormat);
		Payload.Add(0);
		Payload.Append(reinterpret_cast<const uint8*>(&Image.Width), 2);
		Payload.Append(reinterpret_cast<const uint8*>(&Image.Height), 2);
		Payload.Append(reinterpret_cast<const uint8*>(&Image.RawLength), 4);
		const uint32 DataLength = Image.Data.Num();
		Payload.Append(reinterpret_cast<const uint8*>(&DataLength), 4);
		Payload.Append(Image.Data);
	}
	return Payload;
}
} // namespace URSoccerLab
