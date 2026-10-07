#pragma once
#include "Vision/URSImageEncoder.h"
#include "Scene/URSSceneConfig.h"
namespace URSoccerLab
{
// One persistent, serially-used codec per robot/guest. No sockets or UObjects.
class FVideoEncoder
{
  public:
    explicit FVideoEncoder(const FURSRgbStreamConfig &Settings);
    ~FVideoEncoder();
    bool Encode(const TArray<FRawCameraImage> &Images, FEncodedCameraFrame &Frame);

  private:
    struct FImpl;
    TUniquePtr<FImpl> Impl;
};
} // namespace URSoccerLab
