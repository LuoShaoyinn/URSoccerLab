#pragma once

#include "CoreMinimal.h"
#include "Sockets.h"
#include "SocketSubsystem.h"

class URSNonBlockingSocket
{
	FSocket* Sock = nullptr;
public:
	URSNonBlockingSocket() = default;

	bool Listen(int32 Port)
	{
		auto* SSS = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
		Sock = SSS->CreateSocket(NAME_Stream, TEXT("URS"), false);
		if (!Sock) return false;
		Sock->SetReuseAddr();
		Sock->SetNonBlocking(true);
		TSharedRef<FInternetAddr> Addr = SSS->GetLocalBindAddr(*GLog);
		Addr->SetPort(Port);
		if (!Sock->Bind(*Addr)) return false;
		return Sock->Listen(16);
	}

	bool Accept(URSNonBlockingSocket& OutClient)
	{
		if (!Sock) return false;
		auto* SSS = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
		TSharedRef<FInternetAddr> Remote = SSS->CreateInternetAddr();
		FSocket* ClientSock = Sock->Accept(*Remote, TEXT("URS-C"));
		if (!ClientSock) return false;
		ClientSock->SetNonBlocking(true);
		OutClient.Sock = ClientSock;
		return true;
	}

	bool HasNewConnection()
	{
		if (!Sock) return false;
		bool bPending = false;
		Sock->HasPendingConnection(bPending);
		return bPending;
	}

	int32 Recv(uint8* Buf, int32 BufLen)
	{
		if (!Sock) return -1;
		int32 N = 0;
		if (!Sock->Recv(Buf, BufLen, N))
		{
			if (Sock->GetConnectionState() == SCS_ConnectionError) return -1;
			return 0;
		}
		return N;
	}

	int32 Send(const uint8* Data, int32 Len)
	{
		if (!Sock) return -1;
		int32 N = 0;
		if (!Sock->Send(Data, Len, N))
		{
			if (Sock->GetConnectionState() == SCS_ConnectionError) return -1;
			return 0;
		}
		return N;
	}

	bool IsValid() const { return Sock != nullptr; }
	void Close()
	{
		if (Sock)
		{
			ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->DestroySocket(Sock);
			Sock = nullptr;
		}
	}
	~URSNonBlockingSocket() { Close(); }

	// Move only
	URSNonBlockingSocket(URSNonBlockingSocket&& Other) : Sock(Other.Sock) { Other.Sock = nullptr; }
	URSNonBlockingSocket& operator=(URSNonBlockingSocket&& Other)
	{ Close(); Sock = Other.Sock; Other.Sock = nullptr; return *this; }
};
