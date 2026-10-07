#pragma once
#include "Vision/URSImageEncoder.h"
namespace URSoccerLab
{
// Per connection, owned by the socket worker. Never requests a keyframe.
// Pending is a whole unsent message; bytes already in a write buffer stay intact.
struct FVideoDeliveryGate
{
 bool WaitingForKey = true, HaveSequence = false;
 uint64 Epoch = 0;
 uint32 LastSequence = 0;
 bool Offer(const FEncodedCameraFrame& Frame, TArray<uint8>& Pending, const TArray<uint8>& Payload)
 {
  if (!Frame.bVideo) { WaitingForKey = true; HaveSequence = false; Pending = Payload; return true; }
  if (!HaveSequence || Epoch != Frame.VideoEpoch || Frame.Sequence != LastSequence + 1)
  { Pending.Empty(); WaitingForKey = true; }
  // Replacing any unsent encoded packet breaks the dependency chain.
  if (!Pending.IsEmpty()) { Pending.Empty(); WaitingForKey = true; }
  Epoch = Frame.VideoEpoch; LastSequence = Frame.Sequence; HaveSequence = true;
  if (WaitingForKey && !Frame.bKeyFrame) return false;
  WaitingForKey = false;
  Pending = Payload;
  return true;
 }
};
}
