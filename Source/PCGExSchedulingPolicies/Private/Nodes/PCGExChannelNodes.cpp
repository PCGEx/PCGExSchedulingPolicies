// Copyright 2026 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#include "Nodes/PCGExChannelNodes.h"

#include "PCGExSchedulingSubsystem.h"

#include "PCGCommon.h"
#include "PCGContext.h"
#include "PCGGraphExecutionStateInterface.h"
#include "PCGParamData.h"
#include "PCGPin.h"
#include "Metadata/PCGMetadata.h"
#include "Metadata/PCGMetadataAttribute.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(PCGExChannelNodes)

#define LOCTEXT_NAMESPACE "PCGExChannelNodes"

namespace PCGExChannelNodes
{
	const FName BlockedPinLabel = TEXT("Blocked");
	const FName DefaultPinLabel = TEXT("Default");
	const FName ChannelAttributeName = TEXT("Channel");

	struct FResolvedChannels
	{
		/** False when executing outside the runtime generation system (or no subsystem) -- fallback applies. */
		bool bResolved = false;

		/** Union of the channel masks of the generation sources within radius. */
		PCGExScheduling::FChannelMask ActiveMask = 0;

		TSharedPtr<const FPCGExActiveSourcesSnapshot> Snapshot;
	};

	FResolvedChannels ResolveActiveChannels(FPCGContext* InContext, const UPCGExChannelNodeSettings* InSettings)
	{
		FResolvedChannels Result;

		IPCGGraphExecutionSource* ExecutionSource = InContext ? InContext->ExecutionSource.Get() : nullptr;
		if (!ExecutionSource || !InSettings)
		{
			return Result;
		}

		const IPCGGraphExecutionState& State = ExecutionSource->GetExecutionState();
		if (!State.IsManagedByRuntimeGenSystem())
		{
			return Result;
		}

		UPCGExSchedulingSubsystem* Subsystem = UPCGExSchedulingSubsystem::GetInstance(State.GetWorld());
		if (!Subsystem)
		{
			return Result;
		}

		// Runtime-managed with a live subsystem: resolved (possibly to an empty active set).
		Result.bResolved = true;
		Result.Snapshot = Subsystem->GetActiveSourcesSnapshot();
		if (!Result.Snapshot)
		{
			return Result;
		}

		// Local (partitioned) components report their cell bounds -- per-cell accuracy.
		// Bare originals can yield an invalid box: fall back to the execution transform's
		// origin so the radius filter never silently fails open (matching every source).
		const FBox Bounds = State.GetBounds();
		const FVector FallbackOrigin = State.GetTransform().GetLocation();

		double Radius = InSettings->CustomRadius;
		if (InSettings->RadiusMode == EPCGExChannelRadiusMode::GenerationRadius)
		{
			const double GenerationRadius = State.GetGenerationRadiusFromGrid(State.GetGenerationGridSize());
			Radius = GenerationRadius > 0.0 ? GenerationRadius : InSettings->CustomRadius;
		}

		const double RadiusSquared = Radius * Radius;

		for (const FPCGExActiveSourcesSnapshot::FSourceState& Source : Result.Snapshot->Sources)
		{
			const double DistanceSquared = Bounds.IsValid
				                               ? Bounds.ComputeSquaredDistanceToPoint(Source.Position)
				                               : FVector::DistSquared(FallbackOrigin, Source.Position);

			if (DistanceSquared > RadiusSquared)
			{
				continue;
			}

			Result.ActiveMask |= (Source.bIsEditorCamera && InSettings->bEditorCameraMatchesAllChannels)
				                     ? ~PCGExScheduling::FChannelMask(0)
				                     : Source.Mask;
		}

		return Result;
	}

	/** Bitmask with one set bit per output pin (bits = DEACTIVATED pins when written to InactiveOutputPinBitmask). */
	FORCEINLINE uint64 FullPinMask(const int32 NumPins)
	{
		return NumPins >= 64 ? ~uint64(0) : ((uint64(1) << NumPins) - 1);
	}

	/** Local copy of PCGGather::GatherDataForPin -- the engine helper is not exported (no PCG_API). */
	FPCGDataCollection GatherDataForPin(const FPCGDataCollection& InputData, const FName InputLabel, const FName OutputLabel)
	{
		TArray<FPCGTaggedData> GatheredData = InputData.GetInputsByPin(InputLabel);
		FPCGDataCollection Output;

		if (GatheredData.IsEmpty())
		{
			return Output;
		}

		if (GatheredData.Num() == InputData.TaggedData.Num())
		{
			Output = InputData;
		}
		else
		{
			Output.TaggedData = MoveTemp(GatheredData);
		}

		for (FPCGTaggedData& TaggedData : Output.TaggedData)
		{
			TaggedData.Pin = OutputLabel;
		}

		return Output;
	}
}

#pragma region UPCGExChannelGateSettings

#if WITH_EDITOR
FText UPCGExChannelGateSettings::GetDefaultNodeTitle() const
{
	return LOCTEXT("GateNodeTitle", "Channel Gate");
}

FText UPCGExChannelGateSettings::GetNodeTooltipText() const
{
	return LOCTEXT("GateNodeTooltip", "Forwards input data only when the channel condition holds for the generation sources around the executing component. Never cached -- results depend on live world state.");
}

EPCGChangeType UPCGExChannelGateSettings::GetChangeTypeForProperty(const FName& InPropertyName) const
{
	EPCGChangeType ChangeType = Super::GetChangeTypeForProperty(InPropertyName) | EPCGChangeType::Cosmetic;

	if (InPropertyName == GET_MEMBER_NAME_CHECKED(UPCGExChannelGateSettings, bEnabled)
		|| InPropertyName == GET_MEMBER_NAME_CHECKED(UPCGExChannelGateSettings, bOutputBlockedPin))
	{
		ChangeType |= EPCGChangeType::Structural;
	}

	return ChangeType;
}
#endif // WITH_EDITOR

FString UPCGExChannelGateSettings::GetAdditionalTitleInformation() const
{
	return PCGExScheduling::JoinChannelNames(Channels.Channels);
}

TArray<FPCGPinProperties> UPCGExChannelGateSettings::InputPinProperties() const
{
	TArray<FPCGPinProperties> PinProperties;
	PinProperties.Emplace(PCGPinConstants::DefaultInputLabel, EPCGDataType::Any);
	return PinProperties;
}

TArray<FPCGPinProperties> UPCGExChannelGateSettings::OutputPinProperties() const
{
	TArray<FPCGPinProperties> PinProperties;
	PinProperties.Emplace(PCGPinConstants::DefaultOutputLabel,
		EPCGDataType::Any,
		/*bInAllowMultipleConnections=*/true,
		/*bAllowMultipleData=*/true,
		LOCTEXT("GateOutTooltip", "Receives the input data when the channel condition holds."));

	if (bOutputBlockedPin)
	{
		PinProperties.Emplace(PCGExChannelNodes::BlockedPinLabel,
			EPCGDataType::Any,
			/*bInAllowMultipleConnections=*/true,
			/*bAllowMultipleData=*/true,
			LOCTEXT("GateBlockedTooltip", "Receives the input data when the channel condition fails."));
	}

	return PinProperties;
}

FPCGElementPtr UPCGExChannelGateSettings::CreateElement() const
{
	return MakeShared<FPCGExChannelGateElement>();
}

bool FPCGExChannelGateElement::ExecuteInternal(FPCGContext* Context) const
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FPCGExChannelGateElement::ExecuteInternal);

	const UPCGExChannelGateSettings* Settings = Context->GetInputSettings<UPCGExChannelGateSettings>();
	check(Settings);

	const PCGExChannelNodes::FResolvedChannels Resolved = PCGExChannelNodes::ResolveActiveChannels(Context, Settings);

	bool bPass;
	if (!Resolved.bResolved)
	{
		bPass = Settings->Fallback == EPCGExChannelNodeFallback::Pass;
	}
	else
	{
		const PCGExScheduling::FChannelMask WantedMask = Resolved.Snapshot ? Resolved.Snapshot->ResolveNames(Settings->Channels.Channels) : 0;

		switch (Settings->Match)
		{
		case EPCGExChannelMatch::All:
			bPass = WantedMask != 0 && (Resolved.ActiveMask & WantedMask) == WantedMask;
			break;
		case EPCGExChannelMatch::None:
			bPass = (Resolved.ActiveMask & WantedMask) == 0;
			break;
		case EPCGExChannelMatch::Any:
		default:
			bPass = (Resolved.ActiveMask & WantedMask) != 0;
			break;
		}
	}

	// Pins: [Out] (+ [Blocked] when enabled) -- compute the deactivation mask from the pin
	// count and the routed index rather than hard-coding positional literals.
	const int32 NumPins = Settings->bOutputBlockedPin ? 2 : 1;
	uint64 InactiveMask = PCGExChannelNodes::FullPinMask(NumPins);

	if (bPass)
	{
		Context->OutputData = PCGExChannelNodes::GatherDataForPin(Context->InputData, PCGPinConstants::DefaultInputLabel, PCGPinConstants::DefaultOutputLabel);
		InactiveMask &= ~(uint64(1) << 0);
	}
	else if (Settings->bOutputBlockedPin)
	{
		Context->OutputData = PCGExChannelNodes::GatherDataForPin(Context->InputData, PCGPinConstants::DefaultInputLabel, PCGExChannelNodes::BlockedPinLabel);
		InactiveMask &= ~(uint64(1) << 1);
	}

	Context->OutputData.InactiveOutputPinBitmask = InactiveMask;

	return true;
}

#pragma endregion

#pragma region UPCGExChannelSwitchSettings

#if WITH_EDITOR
FText UPCGExChannelSwitchSettings::GetDefaultNodeTitle() const
{
	return LOCTEXT("SwitchNodeTitle", "Channel Switch");
}

FText UPCGExChannelSwitchSettings::GetNodeTooltipText() const
{
	return LOCTEXT("SwitchNodeTooltip", "Routes input data to one output pin per selected channel, based on the channels active around the executing component. Unmatched data goes to 'Default'. Never cached -- results depend on live world state.");
}

EPCGChangeType UPCGExChannelSwitchSettings::GetChangeTypeForProperty(const FName& InPropertyName) const
{
	EPCGChangeType ChangeType = Super::GetChangeTypeForProperty(InPropertyName) | EPCGChangeType::Cosmetic;

	if (InPropertyName == GET_MEMBER_NAME_CHECKED(UPCGExChannelSwitchSettings, bEnabled)
		|| InPropertyName == GET_MEMBER_NAME_CHECKED(UPCGExChannelSwitchSettings, Channels))
	{
		// Channel selection defines the output pins -- structural.
		ChangeType |= EPCGChangeType::Structural;
	}

	return ChangeType;
}
#endif // WITH_EDITOR

FString UPCGExChannelSwitchSettings::GetAdditionalTitleInformation() const
{
	return PCGExScheduling::JoinChannelNames(Channels.Channels);
}

void UPCGExChannelSwitchSettings::GetOutputPinLabels(TArray<FName>& OutLabels) const
{
	OutLabels.Reset(Channels.Channels.Num() + 1);

	// Cap at 63 channel pins so the pin bitmask (incl. 'Default') stays within 64 bits.
	constexpr int32 MaxChannelPins = 63;

	for (const FName& Channel : Channels.Channels)
	{
		// A channel named like the reserved fallback pin must not collide with it -- the
		// engine would drop the duplicate pin while the routing bitmask still indexes both.
		if (Channel.IsNone() || Channel == PCGExChannelNodes::DefaultPinLabel || OutLabels.Contains(Channel))
		{
			continue;
		}

		OutLabels.Add(Channel);

		if (OutLabels.Num() >= MaxChannelPins)
		{
			break;
		}
	}

	OutLabels.Add(PCGExChannelNodes::DefaultPinLabel);
}

TArray<FPCGPinProperties> UPCGExChannelSwitchSettings::InputPinProperties() const
{
	TArray<FPCGPinProperties> PinProperties;
	PinProperties.Emplace(PCGPinConstants::DefaultInputLabel, EPCGDataType::Any);
	return PinProperties;
}

TArray<FPCGPinProperties> UPCGExChannelSwitchSettings::OutputPinProperties() const
{
	TArray<FName> Labels;
	GetOutputPinLabels(Labels);

	TArray<FPCGPinProperties> PinProperties;
	PinProperties.Reserve(Labels.Num());

	for (const FName& Label : Labels)
	{
		PinProperties.Emplace(Label, EPCGDataType::Any);
	}

	return PinProperties;
}

FPCGElementPtr UPCGExChannelSwitchSettings::CreateElement() const
{
	return MakeShared<FPCGExChannelSwitchElement>();
}

bool FPCGExChannelSwitchElement::ExecuteInternal(FPCGContext* Context) const
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FPCGExChannelSwitchElement::ExecuteInternal);

	const UPCGExChannelSwitchSettings* Settings = Context->GetInputSettings<UPCGExChannelSwitchSettings>();
	check(Settings);

	TArray<FName> Labels;
	Settings->GetOutputPinLabels(Labels);
	const int32 DefaultPinIndex = Labels.Num() - 1;

	const PCGExChannelNodes::FResolvedChannels Resolved = PCGExChannelNodes::ResolveActiveChannels(Context, Settings);

	// Selected output pin indices; falls back to 'Default' when nothing matches or outside runtime generation.
	TArray<int32, TInlineAllocator<8>> TargetPins;

	if (Resolved.bResolved && Resolved.Snapshot)
	{
		for (int32 Index = 0; Index < DefaultPinIndex; ++Index)
		{
			const PCGExScheduling::FChannelMask ChannelBit = Resolved.Snapshot->ResolveName(Labels[Index]);
			if (ChannelBit != 0 && (Resolved.ActiveMask & ChannelBit) != 0)
			{
				TargetPins.Add(Index);
				if (!Settings->bRouteToAllMatching)
				{
					break;
				}
			}
		}
	}

	if (TargetPins.IsEmpty())
	{
		TargetPins.Add(DefaultPinIndex);
	}

	// Route the inputs to every selected pin.
	const TArray<FPCGTaggedData> Inputs = Context->InputData.GetInputsByPin(PCGPinConstants::DefaultInputLabel);

	uint64 InactiveMask = PCGExChannelNodes::FullPinMask(Labels.Num());

	for (const int32 PinIndex : TargetPins)
	{
		InactiveMask &= ~(uint64(1) << PinIndex);

		for (const FPCGTaggedData& Input : Inputs)
		{
			FPCGTaggedData& Output = Context->OutputData.TaggedData.Add_GetRef(Input);
			Output.Pin = Labels[PinIndex];
		}
	}

	Context->OutputData.InactiveOutputPinBitmask = InactiveMask;

	return true;
}

#pragma endregion

#pragma region UPCGExGetActiveChannelsSettings

#if WITH_EDITOR
FText UPCGExGetActiveChannelsSettings::GetDefaultNodeTitle() const
{
	return LOCTEXT("GetActiveChannelsNodeTitle", "Get Active Channels");
}

FText UPCGExGetActiveChannelsSettings::GetNodeTooltipText() const
{
	return LOCTEXT("GetActiveChannelsNodeTooltip", "Outputs an attribute set with one 'Channel' entry per channel active around the executing component. Empty outside the runtime generation system. Never cached -- results depend on live world state.");
}
#endif // WITH_EDITOR

TArray<FPCGPinProperties> UPCGExGetActiveChannelsSettings::InputPinProperties() const
{
	return TArray<FPCGPinProperties>();
}

TArray<FPCGPinProperties> UPCGExGetActiveChannelsSettings::OutputPinProperties() const
{
	TArray<FPCGPinProperties> PinProperties;
	PinProperties.Emplace(PCGPinConstants::DefaultOutputLabel, EPCGDataType::Param);
	return PinProperties;
}

FPCGElementPtr UPCGExGetActiveChannelsSettings::CreateElement() const
{
	return MakeShared<FPCGExGetActiveChannelsElement>();
}

bool FPCGExGetActiveChannelsElement::ExecuteInternal(FPCGContext* Context) const
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FPCGExGetActiveChannelsElement::ExecuteInternal);

	const UPCGExGetActiveChannelsSettings* Settings = Context->GetInputSettings<UPCGExGetActiveChannelsSettings>();
	check(Settings);

	UPCGParamData* ParamData = FPCGContext::NewObject_AnyThread<UPCGParamData>(Context);
	FPCGMetadataAttribute<FName>* ChannelAttribute = ParamData->Metadata->CreateAttribute<FName>(PCGExChannelNodes::ChannelAttributeName, NAME_None, /*bAllowsInterpolation=*/false, /*bOverrideParent=*/false);

	const PCGExChannelNodes::FResolvedChannels Resolved = PCGExChannelNodes::ResolveActiveChannels(Context, Settings);

	if (Resolved.bResolved && Resolved.Snapshot && Resolved.Snapshot->ChannelTable && ChannelAttribute)
	{
		for (const TPair<FName, PCGExScheduling::FChannelMask>& Entry : *Resolved.Snapshot->ChannelTable)
		{
			if ((Resolved.ActiveMask & Entry.Value) != 0)
			{
				ChannelAttribute->SetValue(ParamData->Metadata->AddEntry(), Entry.Key);
			}
		}
	}

	FPCGTaggedData& Output = Context->OutputData.TaggedData.Emplace_GetRef();
	Output.Data = ParamData;
	Output.Pin = PCGPinConstants::DefaultOutputLabel;

	return true;
}

#pragma endregion

#undef LOCTEXT_NAMESPACE
