// Copyright Epic Games, Inc. All Rights Reserved.

#include "MMDDemoGameMode.h"
#include "MMDDemoCharacter.h"
#include "UObject/ConstructorHelpers.h"

AMMDDemoGameMode::AMMDDemoGameMode()
{
	// set default pawn class to our Blueprinted character
	static ConstructorHelpers::FClassFinder<APawn> PlayerPawnBPClass(TEXT("/Game/ThirdPersonCPP/Blueprints/ThirdPersonCharacter"));
	if (PlayerPawnBPClass.Class != NULL)
	{
		DefaultPawnClass = PlayerPawnBPClass.Class;
	}
}
