// Copyright 2026 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#pragma once

#include "CoreMinimal.h"
#include "RuntimeGen/SchedulingPolicies/PCGSchedulingPolicyBase.h"
#include "RuntimeGen/SchedulingPolicies/PCGSchedulingPolicyDistanceAndDirection.h" // EPCGSchedulingPolicyNetworkMode

#include "PCGExSchedulingCommon.h"
#include "PCGExSchedulingSubsystem.h"

#include "PCGExSchedulingPolicy.generated.h"

class UPCGExSchedulingConstraint;

/**
 * Composable runtime scheduling policy:
 * - Opt-in channel filtering of generation sources (channels defined in Project Settings →
 *   Plugins → PCGEx | Scheduling Policies; sources carry channels explicitly via the PCGEx
 *   Generation Source component, or through actor tags mapped in the settings).
 * - A combinable stack of scheduling constraints (shapes, source signals, world targets)
 *   refining which grid cells generate, get cleaned up, and in what order -- within the
 *   component's generation radii, which remain the engine-level broadphase.
 *
 * An empty constraint stack passes all gates with priority 0 -- a valid, channel-only layering policy.
 *
 * Threading: ShouldGenerate / CalculatePriority run on the game thread (scan); ShouldCull runs inside
 * a ParallelFor the game thread joins, so it is concurrent and never writes cached state -- see
 * UPCGExSchedulingConstraint.
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

	/** Draws every enabled constraint for the sources this policy reacts to. No-op unless debug drawing is compiled in. Game thread. */
	void DebugDraw(const UWorld* InWorld, const TArray<IPCGGenSourceBase*>& InGenSources, bool bUse2DGrid, bool bDrawCleanup) const;

protected:
	/**
	 * Network mode → editor camera bypass → channel mask test. bAllowCache: scan path only (game
	 * thread, no concurrency) -- enables the per-frame one-entry cache and cached mask refreshes.
	 */
	bool PassesSourceFilters(const IPCGGenSourceBase* InGenSource, bool bAllowCache) const;

	/** Uncached editor camera bypass + channel mask test. */
	bool EvaluateSourceFilter(const IPCGGenSourceBase* InGenSource, bool bAllowCache) const;

	/** Combines constraint gates per CombineMode. bExpanded selects the cleanup (hysteresis) variant. */
	bool EvaluateGates(const IPCGGenSourceBase* InGenSource, const FBox& InBounds, bool bUse2DGrid, bool bExpanded) const;

	/** Resolved mask of Channels. Cached against the settings revision when bAllowCache; otherwise resolved on the spot if stale. */
	PCGExScheduling::FChannelMask GetPolicyChannelMask(bool bAllowCache) const;

private:
	/** Cached resolution of Channels. Written on the scan path only; read anywhere. */
	mutable PCGExScheduling::FChannelMask CachedChannelMask = 0;
	mutable uint32 CachedChannelRevision = 0;

	/**
	 * Scan-path one-entry cache of the source filter, valid for the current frame only: the scan
	 * loops cells per source inside one tick, during which no mask, settings or policy edit can land
	 * and no object can be recycled. Never read on the cleanup path.
	 */
	mutable const IPCGGenSourceBase* CachedFilterSource = nullptr;
	mutable uint64 CachedFilterFrame = 0;
	mutable bool bCachedFilterResult = false;

	/**
	 * Active-sources snapshot grabbed once per frame on the scan path. The cleanup path resolves
	 * source masks from it lock-free (identity lookup over a handful of sources) and only falls back
	 * to the subsystem's locked cache when the snapshot is missing, too old, or lacks the source.
	 */
	mutable TSharedPtr<const FPCGExActiveSourcesSnapshot> CachedSourcesSnapshot;
	mutable uint64 CachedSourcesFrame = 0;
};
