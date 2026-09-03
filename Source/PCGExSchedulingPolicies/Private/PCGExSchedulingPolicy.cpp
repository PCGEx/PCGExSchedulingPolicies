// Copyright 2026 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#include "PCGExSchedulingPolicy.h"

#include "PCGExSchedulingConstraint.h"
#include "PCGExSchedulingSettings.h"
#include "PCGExSchedulingSubsystem.h"

#include "RuntimeGen/GenSources/PCGGenSourceBase.h"

#include "EngineDefines.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(PCGExSchedulingPolicy)

namespace PCGExSchedulingPolicy
{
	/**
	 * Oldest scan-path sources snapshot the cleanup path still trusts (~2 s at 60 fps, the cadence the
	 * subsystem's own mask maintenance offers). Older: fall back to the locked subsystem cache.
	 */
	constexpr uint64 MaxCachedSourcesAgeFrames = 120;
}

double UPCGExSchedulingPolicy::CalculatePriority(const IPCGGenSourceBase* InGenSource, const FBox& GenerationBounds, const bool bUse2DGrid) const
{
	check(InGenSource);

	// Only reached for (source, cell) pairs that passed ShouldGenerate.
	double Priority = 0.0;
	double MaxPriority = 0.0;

	for (const UPCGExSchedulingConstraint* Constraint : Constraints)
	{
		if (!Constraint || !Constraint->bEnabled || Constraint->PriorityWeight <= 0.0f)
		{
			continue;
		}

		const TOptional<double> Contribution = Constraint->CalcPriority(InGenSource, GenerationBounds, bUse2DGrid);
		if (!Contribution.IsSet())
		{
			continue;
		}

		Priority += Constraint->PriorityWeight * FMath::Clamp(Contribution.GetValue(), 0.0, 1.0);
		MaxPriority += Constraint->PriorityWeight;
	}

	return MaxPriority > 0.0 ? Priority / MaxPriority : 0.0;
}

bool UPCGExSchedulingPolicy::ShouldGenerate(const IPCGGenSourceBase* InGenSource, const FBox& GenerationBounds, const bool bUse2DGrid) const
{
	if (!PassesSourceFilters(InGenSource, /*bAllowCache=*/true))
	{
		return false;
	}

	return EvaluateGates(InGenSource, GenerationBounds, bUse2DGrid, /*bExpanded=*/false);
}

bool UPCGExSchedulingPolicy::ShouldCull(const IPCGGenSourceBase* InGenSource, const FBox& GenerationBounds, const bool bUse2DGrid) const
{
	// Filtered-out sources vote to cull, mirroring the stock policy's network mode pattern.
	if (!PassesSourceFilters(InGenSource, /*bAllowCache=*/false))
	{
		return true;
	}

	return !EvaluateGates(InGenSource, GenerationBounds, bUse2DGrid, /*bExpanded=*/true);
}

bool UPCGExSchedulingPolicy::CullsBasedOnDirection() const
{
	for (const UPCGExSchedulingConstraint* Constraint : Constraints)
	{
		if (Constraint && Constraint->bEnabled && Constraint->CullsBasedOnDirection())
		{
			return true;
		}
	}

	return false;
}

bool UPCGExSchedulingPolicy::IsEquivalent(const UPCGSchedulingPolicyBase* OtherSchedulingPolicy) const
{
	if (this == OtherSchedulingPolicy)
	{
		return true;
	}

	if (!Super::IsEquivalent(OtherSchedulingPolicy))
	{
		return false;
	}

	const UPCGExSchedulingPolicy* Other = Cast<UPCGExSchedulingPolicy>(OtherSchedulingPolicy);
	if (!Other)
	{
		return false;
	}

	if (bFilterByChannels != Other->bFilterByChannels
		|| Channels != Other->Channels
		|| UnresolvedSources != Other->UnresolvedSources
		|| bEditorCameraBypassesChannels != Other->bEditorCameraBypassesChannels
		|| NetworkMode != Other->NetworkMode
		|| CombineMode != Other->CombineMode
		|| Constraints.Num() != Other->Constraints.Num())
	{
		return false;
	}

	for (int32 Index = 0; Index < Constraints.Num(); ++Index)
	{
		const UPCGExSchedulingConstraint* Constraint = Constraints[Index];
		const UPCGExSchedulingConstraint* OtherConstraint = Other->Constraints[Index];

		if (Constraint && OtherConstraint)
		{
			if (!Constraint->IsEquivalentTo(OtherConstraint))
			{
				return false;
			}
		}
		else if (Constraint != OtherConstraint)
		{
			return false;
		}
	}

	return true;
}

#if WITH_EDITOR
void UPCGExSchedulingPolicy::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);

	// Force channel mask re-resolution on any edit -- cheap, and covers Channels edits.
	CachedChannelRevision = 0;
	CachedFilterSource = nullptr;
}
#endif

void UPCGExSchedulingPolicy::SetChannels(const FPCGExChannelSelector& InChannels)
{
	Channels = InChannels;
	CachedChannelRevision = 0;
	CachedFilterSource = nullptr;
}

void UPCGExSchedulingPolicy::DebugDraw(const UWorld* InWorld, const TArray<IPCGGenSourceBase*>& InGenSources, const bool bUse2DGrid, const bool bDrawCleanup) const
{
#if UE_ENABLE_DEBUG_DRAWING
	// Only the sources this policy actually reacts to.
	TArray<const IPCGGenSourceBase*, TInlineAllocator<8>> Sources;
	for (const IPCGGenSourceBase* GenSource : InGenSources)
	{
		if (GenSource && PassesSourceFilters(GenSource, /*bAllowCache=*/false))
		{
			Sources.Add(GenSource);
		}
	}

	for (const UPCGExSchedulingConstraint* Constraint : Constraints)
	{
		if (!Constraint || !Constraint->bEnabled)
		{
			continue;
		}

		if (!Constraint->DebugDrawsPerSource())
		{
			Constraint->DebugDraw(InWorld, nullptr, bUse2DGrid, bDrawCleanup);
			continue;
		}

		for (const IPCGGenSourceBase* GenSource : Sources)
		{
			Constraint->DebugDraw(InWorld, GenSource, bUse2DGrid, bDrawCleanup);
		}
	}
#endif
}

bool UPCGExSchedulingPolicy::PassesSourceFilters(const IPCGGenSourceBase* InGenSource, const bool bAllowCache) const
{
	check(InGenSource);

	// Network mode filter (parity with the stock policy).
	const bool bIsLocal = InGenSource->IsLocal();
	if ((NetworkMode == EPCGSchedulingPolicyNetworkMode::Client && !bIsLocal)
		|| (NetworkMode == EPCGSchedulingPolicyNetworkMode::Server && bIsLocal))
	{
		return false;
	}

	if (!bFilterByChannels)
	{
		return true;
	}

	if (bAllowCache && CachedFilterSource == InGenSource && CachedFilterFrame == GFrameCounter)
	{
		return bCachedFilterResult;
	}

	const bool bResult = EvaluateSourceFilter(InGenSource, bAllowCache);

	if (bAllowCache)
	{
		CachedFilterSource = InGenSource;
		CachedFilterFrame = GFrameCounter;
		bCachedFilterResult = bResult;
	}

	return bResult;
}

bool UPCGExSchedulingPolicy::EvaluateSourceFilter(const IPCGGenSourceBase* InGenSource, const bool bAllowCache) const
{
	// Camera identity has a single home on the subsystem -- the node snapshot uses the same test.
	if (bEditorCameraBypassesChannels && UPCGExSchedulingSubsystem::IsEditorCameraSource(InGenSource))
	{
		return true;
	}

	TOptional<PCGExScheduling::FChannelMask> SourceMask;

	if (!bAllowCache)
	{
		// Cleanup: lock-free lookup in the scan's per-frame snapshot.
		const FPCGExActiveSourcesSnapshot* Snapshot = CachedSourcesSnapshot.Get();
		if (Snapshot && GFrameCounter - CachedSourcesFrame <= PCGExSchedulingPolicy::MaxCachedSourcesAgeFrames)
		{
			SourceMask = Snapshot->FindMask(InGenSource);
		}
	}

	if (!SourceMask.IsSet())
	{
		UPCGExSchedulingSubsystem* Subsystem = UPCGExSchedulingSubsystem::GetInstance(GetWorld());
		if (!Subsystem)
		{
			return UnresolvedSources == EPCGExUnresolvedSourceBehavior::Accept;
		}

		if (bAllowCache && CachedSourcesFrame != GFrameCounter)
		{
			CachedSourcesSnapshot = Subsystem->GetActiveSourcesSnapshot();
			CachedSourcesFrame = GFrameCounter;
		}

		SourceMask = Subsystem->GetOrResolveSourceMask(InGenSource);
	}

	if (!SourceMask.IsSet() || SourceMask.GetValue() == 0)
	{
		return UnresolvedSources == EPCGExUnresolvedSourceBehavior::Accept;
	}

	return (SourceMask.GetValue() & GetPolicyChannelMask(bAllowCache)) != 0;
}

bool UPCGExSchedulingPolicy::EvaluateGates(const IPCGGenSourceBase* InGenSource, const FBox& InBounds, const bool bUse2DGrid, const bool bExpanded) const
{
	bool bAnyEnabled = false;

	for (const UPCGExSchedulingConstraint* Constraint : Constraints)
	{
		if (!Constraint || !Constraint->bEnabled)
		{
			continue;
		}

		bAnyEnabled = true;

		const bool bPass = Constraint->EvaluateGate(InGenSource, InBounds, bUse2DGrid, bExpanded);

		if (CombineMode == EPCGExConstraintLogic::All && !bPass)
		{
			return false;
		}

		if (CombineMode == EPCGExConstraintLogic::Any && bPass)
		{
			return true;
		}
	}

	// All mode: nothing failed. Any mode: pass only when the stack is empty/disabled.
	return CombineMode == EPCGExConstraintLogic::All ? true : !bAnyEnabled;
}

PCGExScheduling::FChannelMask UPCGExSchedulingPolicy::GetPolicyChannelMask(const bool bAllowCache) const
{
	const UPCGExSchedulingSettings* Settings = GetDefault<UPCGExSchedulingSettings>();
	const uint32 Revision = Settings->GetRevision();

	if (CachedChannelRevision == Revision)
	{
		return CachedChannelMask;
	}

	const PCGExScheduling::FChannelMask Mask = Settings->ResolveChannelNames(Channels.Channels);

	// The cleanup path is concurrent: resolve on the spot, leave the cache to the next scan.
	if (bAllowCache)
	{
		CachedChannelMask = Mask;
		CachedChannelRevision = Revision;
	}

	return Mask;
}
