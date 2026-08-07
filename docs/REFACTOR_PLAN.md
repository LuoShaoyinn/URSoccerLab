# URSoccerLab Lock-Free Refactor Plan

## Architecture: 3 threads, 3 triple buffers, zero locks

```
┌─────────────────────────────────────────────────────────────┐
│                    PHYSICS THREAD (500 Hz)                    │
│  Runs MuJoCo mj_step. No UE API calls. No locks.             │
│                                                              │
│  READS:  CmdBuf.Front()  →  actuator targets                 │
│  WRITES: StateBuf.Back() →  FRobotSnapshot (qpos/qvel/...)   │
│  READS:  GainBuf.Front() →  kp/kv/damping/mode               │
│  WRITES: GainBuf via mjModel gainprm/biasprm                 │
└──────────────────┬───────────────────────┬───────────────────┘
                   │ StateBuf (triple)     │ GainBuf (triple)
                   ▼                       ▲
┌──────────────────────────────────────────┴───────────────────┐
│                   NETWORK THREAD (60 Hz)                      │
│  TCP send/recv. JSON build/parse. No UE rendering.           │
│                                                              │
│  READS:  StateBuf.Front() →  build state JSON (yyjson)       │
│  WRITES: CmdBuf.Back()    →  parse command JSON (yyjson)     │
│  WRITES: GainBuf.Back()   →  parse gain JSON (yyjson)        │
│  SENDS:  state + camera frames over TCP                      │
│  RECVS:  commands + gain params from TCP                     │
│  CAMERA: dequeues encoded frames from game thread (MPSC)     │
└──────────────────┬───────────────────────────────────────────┘
                   │ Camera frames (MPSC queue)
                   ▼
┌─────────────────────────────────────────────────────────────┐
│                    GAME THREAD (render FPS)                   │
│  UE rendering + camera capture only. No TCP. No JSON.        │
│                                                              │
│  Requests SceneCapture readback (UE API)                     │
│  Encodes to JPEG (async thread pool)                         │
│  Pushes encoded frames to network thread (MPSC)              │
│  Updates UE actor transforms from physics snapshot           │
└─────────────────────────────────────────────────────────────┘
```

## New files

```
Source/URSoccerLab/
├── Public/
│   ├── Core/
│   │   ├── URSTripleBuffer.h     ← standalone template, zero deps
│   │   ├── URSBuffers.h          ← FCommandSet, FGainSet POD structs
│   │   ├── URSSnapshot.h         ← FRobotSnapshot (already exists)
│   │   └── URSRobotCoreComponent.h
│   ├── Network/
│   │   ├── URSNetworkThread.h
│   │   ├── URSJson.h
│   │   └── URSSocket.h
│   └── Transport/
│       └── URSTcpTransportComponent.h
├── Private/
│   ├── Core/
│   │   └── URSRobotCoreComponent.cpp
│   ├── Network/
│   │   ├── URSNetworkThread.cpp
│   │   ├── URSJson.cpp
│   │   └── URSSocket.cpp
│   └── Transport/
│       └── URSTcpTransportComponent.cpp
└── ThirdParty/
    └── yyjson/  (already vendored)
```

## Data structures

### URSTripleBuffer (standalone template)
```cpp
template <typename T>
class URSTripleBuffer {
    T Items[3];
    std::atomic<int32> Published;
    int32 BackIdx, FrontIdx;
    T& Back();                    // producer
    void Publish();               // producer
    const T& Front() const;       // consumer
    bool IsNew() const;           // consumer
};
```

### POD structs
```cpp
struct FCommandSet {              // network → physics
    float Targets[40]; double TimestampSec; bool bValid;
};
struct FGainSet {                 // network → physics (rare)
    double Kp[40], Kv[40], Damping[40]; int32 Mode; bool bValid;
};
// FRobotSnapshot already in URSSnapshot.h
```

## Key functions

### Physics thread (URSRobotCoreComponent::PreStepPhysics)
```
1. PublishSnapshot(Model, Data)   → StateBuf.Back() + Publish()
2. ApplyCommands()                → CmdBuf.Front()
3. ApplyGains(Model)              → GainBuf.Front() if dirty
4. ApplyPoseLocks(Model, Data)    → rare, under CallbackMutex
```

### Network thread (URSNetworkThread::Run)
```
loop at StateRateHz:
  1. Recv from all robot sockets (non-blocking)
  2. Parse commands (yyjson) → CmdBuf.Back() + Publish()
  3. Parse gains (yyjson) → GainBuf.Back() + Publish()
  4. Read StateBuf.Front() → build state JSON (yyjson) → TCP send
  5. Drain camera MPSC queue → TCP send
  6. Flush TCP (non-blocking)
```

### Game thread (URSTcpTransportComponent::TickComponent)
```
1. Request SceneCapture readback (UE API)
2. ConsumeCameraFrame → JPEG encode (async) → push to network MPSC queue
3. Update UE actor transforms (URLab handles this)
```

## Implementation order

1. URSTripleBuffer.h
2. URSBuffers.h
3. URSJson.h/.cpp (yyjson)
4. URSSocket.h/.cpp
5. URSNetworkThread.h/.cpp
6. Rewrite URSRobotCoreComponent
7. Rewrite URSTcpTransportComponent
8. Test

## What gets deleted

- FURSRobotState (replaced by FRobotSnapshot + metadata)
- GetRobotState()
- FRWLock EndpointLock
- All FScopeLock/FReadScopeLock/FWriteScopeLock
- BuildStateJson on game thread
- ProcessCommand on game thread
- TickStatePublish / TickCameraPublish / FlushAllWrites on game thread
- UE FJsonSerializer / FJsonObject / MakeShared for state
