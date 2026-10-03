#pragma once
#include "Inspector/URSInspectorProtocol.h"
#include "Vision/URSImageEncoder.h"
// GT-facing contract. Implementations own sockets and framing, never UObjects.
class IURSInspectorNetwork
{
public:
 virtual ~IURSInspectorNetwork() = default;
 virtual bool Start(int32 Port) = 0;
 virtual void Stop() = 0;
 virtual bool Dequeue(URSoccerLab::FInspectorEvent& Event) = 0;
 virtual void Reply(uint64 Session, const FString& Json) = 0;
 virtual void Send(uint64 Session, const URSoccerLab::FEncodedCameraFrame& Frame) = 0;
};
URSOCCERLAB_API TUniquePtr<IURSInspectorNetwork> CreateURSInspectorTcp();
