// Copyright 2026 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#pragma once

#include "CoreMinimal.h"
#include "UObject/Interface.h"

#include "PCGExChannelProvider.generated.h"

UINTERFACE(MinimalAPI, meta = (CannotImplementInterfaceInBlueprint))
class UPCGExChannelProvider : public UInterface
{
	GENERATED_BODY()
};

/**
 * Opt-in channel broadcasting for generation sources.
 * Any IPCGGenSourceBase implementer (ours or third-party) that also implements this
 * interface resolves its scheduling channels through it -- taking precedence over the
 * tag-based fallbacks. Queried on the game thread.
 */
class PCGEXSCHEDULINGPOLICIES_API IPCGExChannelProvider
{
	GENERATED_BODY()

public:
	/** Channel names this generation source broadcasts on (resolved against the scheduling settings). */
	virtual void GetSchedulingChannels(TArray<FName>& OutChannels) const = 0;
};
