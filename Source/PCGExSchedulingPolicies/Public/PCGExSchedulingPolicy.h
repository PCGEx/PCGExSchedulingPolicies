// Copyright 2026 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#pragma once

#include "CoreMinimal.h"
#include "RuntimeGen/SchedulingPolicies/PCGSchedulingPolicyBase.h"
#include "RuntimeGen/SchedulingPolicies/PCGSchedulingPolicyDistanceAndDirection.h" // EPCGSchedulingPolicyNetworkMode

#include "PCGExSchedulingCommon.h"

#include "PCGExSchedulingPolicy.generated.h"

class UPCGExSchedulingConstraint;

/**
 * Composable runtime scheduling policy:
 * - Opt-in channel filtering of generation sources (channels defined in Project Settings →
 *   Plugins → PCGEx | Scheduling Policies; sources carry channels explicitly via the PCGEx
 *   Generation Source component, or through actor tags mapped in the settings).
 * - A combinable stack of scheduling constraints (shapes, source signals, world targets)
 *   refining which grid cells generate, get cleaned up, and in what order — within the
 *   component's generation radii, which remain the engine-level broadphase.
 *
 * An empty constraint stack passes all gates with priority 0 — a valid, channel-only layering policy.
 */
UCLASS(BlueprintType, Blueprintable, ClassGroup = (Procedural), DisplayName = "PCGEx Scheduling Policy")
class PCGEXSCHEDULINGPOLICIES_API UPCGExSchedulingPolicy : public UPCGSchedulingPolicyBase
{
	GENERATED_BODY()

public:
	//~ Begin UPCGSchedulingPolicyBase interface
	virtual double CalculatePriority(const IPCGGenSourceBase* InGenSource, const FBox& GenerationBounds, bool bUse2DGrid) const override;
	virtual bool ShouldGenerate(const IPCGGenSourceBase* InGenSource, const FBox& GenerationBounds, bool bUse2DGrid) const override;
	virtual bool ShouldCull(const IPCGGenSourceBase* InGenSource, const FBox& GenerationBounds, bool bUse2DGrid) const override;
	virtual bool CullsBasedOnDirection() const override;
	virtual bool IsEquivalent(const UPCGSchedulingPolicyBase* OtherSchedulingPolicy) const override;
	//~ End UPCGSchedulingPolicyBase interface

#if WITH_EDITOR
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
#endif

	/** Only react to generation sources whose channels overlap this policy's selection. */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "RuntimeGeneration|Channels", meta = (EditCondition = "bShouldDisplayProperties", EditConditionHides, HideEditConditionToggle))
	bool bFilterByChannels = false;

	/** Channels this policy listens to. An empty selection matches nothing (only unresolved sources set to Accept, and the editor camera bypass, can trigger). */
	UPROPERTY(BlueprintReadOnly, EditAnywhere, Category = "RuntimeGeneration|Channels", meta = (EditCondition = "bShouldDisplayProperties && bFilterByChannels", EditConditionHides))
	FPCGExChannelSelector Channels;

	/** How sources that resolve to no channels at all are treated. */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "RuntimeGeneration|Channels", meta = (EditCondition = "bShouldDisplayProperties && bFilterByChannels", EditConditionHides))
	EPCGExUnresolvedSourceBehavior UnresolvedSources = EPCGExUnresolvedSourceBehavior::Reject;

	/** Let the editor viewport generation source bypass channel filtering, so in-editor preview always works. */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "RuntimeGeneration|Channels", meta = (EditCondition = "bShouldDisplayProperties && bFilterByChannels", EditConditionHides))
	bool bEditorCameraBypassesChannels = true;

	/** Client/Server mode determines which generation sources to consider (parity with the stock policy). */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "RuntimeGeneration|Scheduling Policy Parameters", meta = (EditCondition = "bShouldDisplayProperties", EditConditionHides, HideEditConditionToggle))
	EPCGSchedulingPolicyNetworkMode NetworkMode = EPCGSchedulingPolicyNetworkMode::All;

	/** How the constraint gates combine. */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "RuntimeGeneration|Scheduling Policy Parameters", meta = (EditCondition = "bShouldDisplayProperties", EditConditionHides, HideEditConditionToggle))
	EPCGExConstraintLogic CombineMode = EPCGExConstraintLogic::All;

	/** Composable constraint stack. Gates combine per CombineMode; priorities blend as a weighted normalized sum. */
	UPROPERTY(BlueprintReadOnly, EditAnywhere, Instanced, Category = "RuntimeGeneration|Scheduling Policy Parameters", meta = (EditCondition = "bShouldDisplayProperties", EditConditionHides, HideEditConditionToggle))
	TArray<TObjectPtr<UPCGExSchedulingConstraint>> Constraints;

	/** Sets the listened channels and invalidates the cached channel mask. */
	UFUNCTION(BlueprintCallable, Category = "RuntimeGeneration|Channels")
	void SetChannels(const FPCGExChannelSelector& InChannels);

protected:
	/** Network mode → editor camera bypass → channel mask test. Worker-safe (cached mask reads on non-game threads). */
	bool PassesSourceFilters(const IPCGGenSourceBase* InGenSource) const;

	/** Combines constraint gates per CombineMode. bExpanded selects the cleanup (hysteresis) variant. */
	bool EvaluateGates(const IPCGGenSourceBase* InGenSource, const FBox& InBounds, bool bUse2DGrid, bool bExpanded) const;

	/** Resolved mask of Channels, cached against the settings revision. */
	PCGExScheduling::FChannelMask GetPolicyChannelMask() const;

private:
	/**
	 * Cached resolution of Channels. Written on the game thread (scan phase), read possibly
	 * one tick stale by worker-thread cleanup — ordering is guaranteed by the scheduler
	 * running its cleanup ParallelFor after the game-thread scan within the same tick.
	 */
	mutable PCGExScheduling::FChannelMask CachedChannelMask = 0;
	mutable uint32 CachedChannelRevision = 0;
};
