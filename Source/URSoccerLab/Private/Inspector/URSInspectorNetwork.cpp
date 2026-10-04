#include "Inspector/URSInspectorNetwork.h"
#include "Network/URSSocket.h"
#include "Vision/URSVideoDeliveryGate.h"
#include "Network/URSTcpFraming.h"
#include "Protocol/URSMessageProtocol.h"
#include "HAL/Runnable.h"
#include "HAL/RunnableThread.h"
#include "HAL/PlatformProcess.h"
#include "Misc/ScopeLock.h"
#include "Containers/Queue.h"
#include <atomic>
namespace
{
class FInspectorTcp final : public IURSInspectorNetwork, public FRunnable
{
 struct FClient
 {
  uint64 Id; URSNonBlockingSocket Socket;
  URSoccerLab::FVideoDeliveryGate VideoGate;
  TArray<uint8> Read, Write, PendingImage;
  int32 WriteOffset = 0; bool PendingPose = false;
  double NextPose = 0;
 };
 struct FReply { uint64 Id; FString Json; };
 URSNonBlockingSocket Listener;
 TArray<TUniquePtr<FClient>> Clients;
 TQueue<URSoccerLab::FInspectorEvent, EQueueMode::Spsc> Events;
 TQueue<FReply, EQueueMode::Spsc> Replies;
 FCriticalSection FrameMutex;
 TMap<uint64, URSoccerLab::FEncodedCameraFrame> Frames;
 TSet<uint64> LiveIds;
 std::atomic<bool> Running{false};
 std::atomic<int32> PendingEvents{0};
 FRunnableThread* Thread = nullptr;
 int32 Capacity = 4;
 int32 Port = 12000; uint64 NextId = 1;
 void Event(URSoccerLab::FInspectorEvent::EKind Kind, uint64 Id, const URSoccerLab::FInspectorPose& Pose = {})
 { PendingEvents.fetch_add(1); Events.Enqueue({Kind, Id, Pose}); }
 void Json(FClient& C, const FString& Message)
 {
  FTCHARToUTF8 Bytes(*Message);
  URSoccerLab::TcpFraming::Append(C.Write, 0, reinterpret_cast<const uint8*>(Bytes.Get()), Bytes.Length());
 }
 void Remove(int32 Index)
 {
  const uint64 Id = Clients[Index]->Id;
  { FScopeLock Lock(&FrameMutex); LiveIds.Remove(Id); Frames.Remove(Id); }
  Event(URSoccerLab::FInspectorEvent::EKind::Disconnected, Id);
  Clients.RemoveAt(Index);
 }
public:
 ~FInspectorTcp() override { Stop(); }
 bool Start(int32 InPort, int32 MaxGuests) override
 {
  Stop(); if (InPort < 1 || InPort > 65535) return false;
  if (MaxGuests < 1 || MaxGuests > 4) return false;
  Capacity = MaxGuests; Port = InPort; Running = true;
  Thread = FRunnableThread::Create(this, TEXT("URSInspectorTCP"));
  return Thread && Running;
 }
 bool Init() override
 {
  if (!Listener.Listen(Port)) { Listener.Close(); Running = false; return false; }
  UE_LOG(LogTemp, Log, TEXT("[URS Inspector] listening on port %d"), Port); return true;
 }
 void Stop() override
 {
  Running = false;
  if (Thread) { Thread->WaitForCompletion(); delete Thread; Thread = nullptr; }
  Events.Empty(); Replies.Empty(); PendingEvents = 0;
  FScopeLock Lock(&FrameMutex); Frames.Empty(); LiveIds.Empty();
 }
 bool Dequeue(URSoccerLab::FInspectorEvent& Out) override
 { if (!Events.Dequeue(Out)) return false; PendingEvents.fetch_sub(1); return true; }
 void Reply(uint64 Id, const FString& Message) override { Replies.Enqueue({Id, Message}); }
 void Send(uint64 Id, const URSoccerLab::FEncodedCameraFrame& Frame) override
 { FScopeLock Lock(&FrameMutex); if (LiveIds.Contains(Id)) Frames.Add(Id, Frame); }
 uint32 Run() override
 {
  while (Running)
  {
   // Bound all work and handoffs. Admission also waits for GT to drain lifecycle events.
   for (int N = 0; N < 8 && Listener.HasNewConnection(); ++N)
   {
    auto C = MakeUnique<FClient>(); if (!Listener.Accept(C->Socket)) break;
    if (Clients.Num() >= Capacity || PendingEvents.load() >= 32) continue;
    C->Id = NextId++;
    { FScopeLock Lock(&FrameMutex); LiveIds.Add(C->Id); }
    Event(URSoccerLab::FInspectorEvent::EKind::Connected, C->Id);
    Clients.Add(MoveTemp(C));
   }
   FReply R;
   while (Replies.Dequeue(R)) for (auto& C : Clients) if (C->Id == R.Id)
   { C->PendingPose = false; Json(*C, R.Json); }
   TMap<uint64, URSoccerLab::FEncodedCameraFrame> NewFrames;
   { FScopeLock Lock(&FrameMutex); Swap(NewFrames, Frames); }
   for (auto& Pair : NewFrames) for (auto& C : Clients) if (C->Id == Pair.Key)
    C->VideoGate.Offer(Pair.Value, C->PendingImage, URSoccerLab::FMessageProtocol::EncodeCamera(Pair.Value));
   for (int Index = Clients.Num()-1; Index >= 0; --Index)
   {
    auto& C = *Clients[Index]; bool Alive = true; uint8 Bytes[16384];
    for (int N = 0; N < 4; ++N)
    {
     const int Got = C.Socket.Recv(Bytes, sizeof(Bytes));
     if (Got < 0) { Alive = false; break; } if (!Got) break;
     C.Read.Append(Bytes, Got); if (C.Read.Num() > 65540) { Alive = false; break; }
    }
    for (int N = 0; Alive && N < 8 && C.Read.Num() >= 4; ++N)
    {
     const uint32 Length = URSoccerLab::TcpFraming::ReadLength(C.Read.GetData());
     if (!Length || Length > 4096) { Alive = false; break; }
     if (C.Read.Num() < int32(Length)+4) break;
     FString Error; URSoccerLab::FInspectorPose Pose;
     const double Now = FPlatformTime::Seconds();
     if (C.Read[4] != 0) Error = TEXT("only camera JSON requests are allowed");
     else if (C.PendingPose || Now < C.NextPose) Error = TEXT("camera update busy; limit 30 requests per second");
     else
     {
      FUTF8ToTCHAR Text(reinterpret_cast<const ANSICHAR*>(C.Read.GetData()+5), Length-1);
      if (URSoccerLab::FInspectorProtocol::Decode(FString(Text.Length(), Text.Get()), Pose, Error))
      { C.PendingPose = true; C.NextPose = Now+1.0/30; Event(URSoccerLab::FInspectorEvent::EKind::Pose, C.Id, Pose); }
     }
     if (!Error.IsEmpty()) Json(C, URSoccerLab::FInspectorProtocol::Reply(false, Error));
     C.Read.RemoveAt(0, Length+4, EAllowShrinking::No);
    }
    // Never discard bytes of a partially sent frame. Replace only PendingImage.
    if (C.Write.IsEmpty() && !C.PendingImage.IsEmpty())
    { URSoccerLab::TcpFraming::Append(C.Write, 1, C.PendingImage.GetData(), C.PendingImage.Num()); C.PendingImage.Empty(); }
    if (C.Write.Num() > 16*1024*1024) Alive = false;
    if (Alive && C.Write.Num() > C.WriteOffset)
    {
     const int Sent = C.Socket.Send(C.Write.GetData()+C.WriteOffset, C.Write.Num()-C.WriteOffset);
     if (Sent < 0) Alive = false; else C.WriteOffset += Sent;
     if (C.WriteOffset == C.Write.Num()) { C.Write.Empty(); C.WriteOffset = 0; }
    }
    if (!Alive) Remove(Index);
   }
   FPlatformProcess::Sleep(0.001f);
  }
  return 0;
 }
 void Exit() override { Clients.Empty(); Listener.Close(); }
};
}
TUniquePtr<IURSInspectorNetwork> CreateURSInspectorTcp() { return MakeUnique<FInspectorTcp>(); }
