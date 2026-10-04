#pragma once
#include "Vision/URSImageEncoder.h"
#include "Scene/URSSceneConfig.h"
namespace URSoccerLab
{
// One persistent, serially-used codec per robot/guest. No sockets or UObjects.
class FAv1Encoder
{
public:
 explicit FAv1Encoder(const FURSRgbStreamConfig& Settings);
 ~FAv1Encoder();
 bool Encode(const TArray<FRawCameraImage>& Images, FEncodedCameraFrame& Frame);
private:
 struct FImpl;
 TUniquePtr<FImpl> Impl;
};
}
