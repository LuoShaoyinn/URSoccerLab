#pragma once

#include "CoreMinimal.h"
#include "GameFramework/GameModeBase.h"
#include "URSSoccerGameMode.generated.h"

UCLASS()
class URSOCCERLAB_API AURSSoccerGameMode : public AGameModeBase
{
	GENERATED_BODY()

public:
	virtual void InitGame(const FString& MapName, const FString& Options, FString& ErrorMessage) override;
};
