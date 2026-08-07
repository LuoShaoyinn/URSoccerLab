#pragma once

// URSSocket.h — thin non-blocking TCP wrapper for the network thread.

#include "CoreMinimal.h"
#include "Sockets.h"

class URSNonBlockingSocket
{
	FSocket* Socket = nullptr;

public:
	URSNonBlockingSocket() = default;

	bool Listen(int32 Port)
	{
		ISocketSubsystem* SSS = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
		Socket = SSS->CreateSocket(NAME_Stream, TEXT("URS"), false);
		if (!Socket) return false;

		Socket->SetReuseAddr();
		Socket->SetNonBlocking(true);

		TSharedRef<FInternetAddr> Addr = SSS->GetInternetAddr(0, Port);
		if (!Socket->Bind(*Addr)) return false;
		return Socket->Listen(16);
	}

	bool Accept(URSNonBlockingSocket& OutClient)
	{
		if (!Socket) return false;
		ISocketSubsystem* SSS = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
		TSharedRef<FInternetAddr> ClientAddr = SSS->CreateInternetAddr();
		FSocket* ClientSock = Socket->Accept(*ClientAddr, TEXT("URS-Client"));
		if (!ClientSock) return false;
		ClientSock->SetNonBlocking(true);
		OutClient.Socket = ClientSock;
		return true;
	}

	bool Connect(const FString& Host, int32 Port)
	{
		ISocketSubsystem* SSS = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
		Socket = SSS->CreateSocket(NAME_Stream, TEXT("URS"), false);
		if (!Socket) return false;
		Socket->SetNonBlocking(true);
		TSharedRef<FInternetAddr> Addr = SSS->GetInternetAddr(*Host, Port);
		return Socket->Connect(*Addr);
	}

	// Returns bytes read, 0 if no data, -1 on error.
	int32 Recv(uint8* Buf, int32 BufLen)
	{
		if (!Socket) return -1;
		int32 BytesRead = 0;
		if (!Socket->Recv(Buf, BufLen, BytesRead, ESocketReceiveFlags::NonBlocking))
		{
			ESocketConnectionState State = Socket->GetConnectionState();
			if (State == SCS_ConnectionError) return -1;
			return 0;
		}
		return BytesRead;
	}

	// Returns bytes sent, 0 if would block, -1 on error.
	int32 Send(const uint8* Data, int32 Len)
	{
		if (!Socket) return -1;
		int32 BytesSent = 0;
		if (!Socket->Send(Data, Len, BytesSent))
		{
			ESocketConnectionState State = Socket->GetConnectionState();
			if (State == SCS_ConnectionError) return -1;
			return 0;
		}
		return BytesSent;
	}

	bool HasNewConnection()
	{
		if (!Socket) return false;
		bool bHasPending = false;
		Socket->HasPendingConnection(bHasPending);
		return bHasPending;
	}

	bool IsValid() const { return Socket != nullptr; }
	void Close()
	{
		if (Socket)
		{
			ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->DestroySocket(Socket);
			Socket = nullptr;
		}
	}

	~URSNonBlockingSocket() { Close(); }
};
