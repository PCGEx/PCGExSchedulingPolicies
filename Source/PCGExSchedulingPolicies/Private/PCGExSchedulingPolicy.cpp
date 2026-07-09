// Copyright 2026 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#include "PCGExSchedulingPolicy.h"

#include "PCGExSchedulingConstraint.h"
#include "PCGExSchedulingSettings.h"
#include "PCGExSchedulingSubsystem.h"

#include "RuntimeGen/GenSources/PCGGenSourceBase.h"
#include "RuntimeGen/GenSources/PCGGenSourceEditorCamera.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(PCGExSchedulingPolicy)

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
	if (!PassesSourceFilters(InGenSource))
	{
		return false;
	}

	return EvaluateGates(InGenSource, GenerationBounds, bUse2DGrid, /*bExpanded=*/false);
}

bool UPCGExSchedulingPolicy::ShouldCull(const IPCGGenSourceBase* InGenSource, const FBox& GenerationBounds, const bool bUse2DGrid) const
{
	// Filtered-out sources vote to cull, mirroring the stock policy's network mode pattern.
	if (!PassesSourceFilters(InGenSource))
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

	// Force channel mask re-resolution on any edit — cheap, and covers Channels edits.
	CachedChannelRevision = 0;
}
#endif

void UPCGExSchedulingPolicy::SetChannels(const FPCGExChannelSelector& InChannels)
{
	Channels = InChannels;
	CachedChannelRevision = 0;
}

bool UPCGExSchedulingPolicy::PassesSourceFilters(const IPCGGenSourceBase* InGenSource) const
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

	if (bEditorCameraBypassesChannels && Cast<UPCGGenSourceEditorCamera>(InGenSource))
	{
		return true;
	}

	UPCGExSchedulingSubsystem* Subsystem = UPCGExSchedulingSubsystem::GetInstance(GetWorld());
	if (!Subsystem)
	{
		return UnresolvedSources == EPCGExUnresolvedSourceBehavior::Accept;
	}

	const TOptional<PCGExScheduling::FChannelMask> SourceMask = Subsystem->GetOrResolveSourceMask(InGenSource);
	if (!SourceMask.IsSet() || SourceMask.GetValue() == 0)
	{
		return UnresolvedSources == EPCGExUnresolvedSourceBehavior::Accept;
	}

	return (SourceMask.GetValue() & GetPolicyChannelMask()) != 0;
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

PCGExScheduling::FChannelMask UPCGExSchedulingPolicy::GetPolicyChannelMask() const
{
	const UPCGExSchedulingSettings* Settings = GetDefault<UPCGExSchedulingSettings>();
	const uint32 Revision = Settings->GetRevision();

	if (IsInGameThread())
	{
		if (CachedChannelRevision != Revision)
		{
			CachedChannelMask = Settings->ResolveChannelNames(Channels.Channels);
			CachedChannelRevision = Revision;
		}
	}

	// Worker threads read the (possibly one-tick stale) cached value — see member comment.
	return CachedChannelMask;
}
