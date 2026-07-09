// Copyright 2026 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#pragma once

#include "CoreMinimal.h"

#include "PCGExSchedulingCommon.generated.h"

PCGEXSCHEDULINGPOLICIES_API DECLARE_LOG_CATEGORY_EXTERN(LogPCGExScheduling, Log, All);

namespace PCGExScheduling
{
	/** Hard cap on the number of scheduling channels -- channel sets resolve to a 64-bit mask. */
	inline constexpr int32 MaxChannels = 64;

	/** Runtime representation of a set of channels. Bit indices map to entries in UPCGExSchedulingSettings::Channels. */
	using FChannelMask = uint64;

	/** Inclusive 1D interval overlap. */
	FORCEINLINE bool IntervalsOverlap(const double MinA, const double MaxA, const double MinB, const double MaxB)
	{
		return MinA <= MaxB && MinB <= MaxA;
	}

	/** Comma-joined channel names for UI summaries (node subtitles, picker labels). */
	inline FString JoinChannelNames(const TArray<FName>& InNames)
	{
		return FString::JoinBy(InNames, TEXT(", "), [](const FName& Name) { return Name.ToString(); });
	}
}

/** How channel-filtered policies treat generation sources that resolve to no channels at all. */
UENUM(BlueprintType)
enum class EPCGExUnresolvedSourceBehavior : uint8
{
	Reject = 0 UMETA(ToolTip = "Sources without any channel never trigger this policy."),
	Accept = 1 UMETA(ToolTip = "Sources without any channel always trigger this policy, as if channel filtering was disabled for them.")
};

/** How multiple scheduling constraints combine their gates. */
UENUM(BlueprintType)
enum class EPCGExConstraintLogic : uint8
{
	All = 0 UMETA(ToolTip = "Every enabled constraint must pass for a cell to generate."),
	Any = 1 UMETA(ToolTip = "At least one enabled constraint must pass for a cell to generate.")
};

/**
 * A set of scheduling channel names, as defined in Project Settings → Plugins → PCGEx | Scheduling Policies.
 * Serialized by name so reordering or renaming channels in the settings never silently re-targets existing selectors.
 */
USTRUCT(BlueprintType)
struct PCGEXSCHEDULINGPOLICIES_API FPCGExChannelSelector
{
	GENERATED_BODY()

	/** Selected channel names. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = Settings)
	TArray<FName> Channels;

	bool IsEmpty() const { return Channels.IsEmpty(); }

	/** Exact (order-sensitive) comparison -- conservative equality used by policy equivalence checks. */
	bool operator==(const FPCGExChannelSelector& Other) const { return Channels == Other.Channels; }
	bool operator!=(const FPCGExChannelSelector& Other) const { return !(*this == Other); }
};
