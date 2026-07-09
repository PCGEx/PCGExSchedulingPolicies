// Copyright 2026 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#pragma once

#include "CoreMinimal.h"
#include "PCGSettings.h"

#include "PCGExSchedulingCommon.h"

#include "PCGExChannelNodes.generated.h"

/** How a set of channels is matched against the channels active around the executing component. */
UENUM(BlueprintType)
enum class EPCGExChannelMatch : uint8
{
	Any = 0 UMETA(ToolTip = "Passes when at least one selected channel is active."),
	All = 1 UMETA(ToolTip = "Passes when every selected channel is active."),
	None = 2 UMETA(ToolTip = "Passes when no selected channel is active.")
};

/** Behavior when the graph executes outside the runtime generation system (editor generation, Generate On Demand/Load). */
UENUM(BlueprintType)
enum class EPCGExChannelNodeFallback : uint8
{
	Pass = 0 UMETA(ToolTip = "Data passes through, as if the condition matched."),
	Block = 1 UMETA(ToolTip = "Data is blocked, as if the condition failed.")
};

/** Which radius gathers the generation sources considered 'active' for this component. */
UENUM(BlueprintType)
enum class EPCGExChannelRadiusMode : uint8
{
	GenerationRadius = 0 UMETA(ToolTip = "The component's runtime generation radius for its grid. Falls back to Custom Radius when it cannot be resolved."),
	Custom = 1 UMETA(ToolTip = "An explicit radius around the executing component's bounds.")
};

/**
 * Base for channel-aware graph nodes: gathers the channels of the generation sources
 * around the executing component (the sources 'behind' this execution).
 * Results depend on live world state — these nodes are never cached.
 */
UCLASS(Abstract, ClassGroup = (Procedural))
class PCGEXSCHEDULINGPOLICIES_API UPCGExChannelNodeSettings : public UPCGSettings
{
	GENERATED_BODY()

public:
	/** Which radius gathers nearby generation sources. */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = Settings)
	EPCGExChannelRadiusMode RadiusMode = EPCGExChannelRadiusMode::GenerationRadius;

	/** Radius around the executing component's bounds. */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = Settings, meta = (EditCondition = "RadiusMode == EPCGExChannelRadiusMode::Custom", EditConditionHides, ClampMin = 0.0, Units = "cm"))
	double CustomRadius = 25600.0;

	/** Treat the editor viewport generation source as broadcasting on every channel, so in-editor runtime preview always matches. */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = Settings)
	bool bEditorCameraMatchesAllChannels = true;

protected:
	//~ Begin UPCGSettings interface
	virtual bool HasExecutionDependencyPin() const override { return false; }
	//~ End UPCGSettings interface
};

/**
 * Forwards input data only when the channel condition holds for the generation sources
 * around the executing component. Optionally routes rejected data to a 'Blocked' pin.
 */
UCLASS(BlueprintType, ClassGroup = (Procedural), meta = (Keywords = "channel gate filter allow deny layer"))
class PCGEXSCHEDULINGPOLICIES_API UPCGExChannelGateSettings : public UPCGExChannelNodeSettings
{
	GENERATED_BODY()

public:
	//~ Begin UPCGSettings interface
#if WITH_EDITOR
	virtual FName GetDefaultNodeName() const override { return FName(TEXT("ChannelGate")); }
	virtual FText GetDefaultNodeTitle() const override;
	virtual FText GetNodeTooltipText() const override;
	virtual EPCGSettingsType GetType() const override { return EPCGSettingsType::ControlFlow; }
#endif
	virtual FString GetAdditionalTitleInformation() const override;
	virtual bool HasDynamicPins() const override { return true; }
	virtual bool OutputPinsCanBeDeactivated() const override { return true; }

protected:
#if WITH_EDITOR
	virtual EPCGChangeType GetChangeTypeForProperty(const FName& InPropertyName) const override;
#endif
	virtual TArray<FPCGPinProperties> InputPinProperties() const override;
	virtual TArray<FPCGPinProperties> OutputPinProperties() const override;
	virtual FPCGElementPtr CreateElement() const override;
	//~ End UPCGSettings interface

public:
	/** Channels to test. */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = Settings)
	FPCGExChannelSelector Channels;

	/** How the selected channels are matched against the active set. */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = Settings)
	EPCGExChannelMatch Match = EPCGExChannelMatch::Any;

	/** Behavior when executing outside the runtime generation system. */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = Settings)
	EPCGExChannelNodeFallback Fallback = EPCGExChannelNodeFallback::Pass;

	/** Route rejected data to a 'Blocked' output pin instead of discarding it. */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = Settings)
	bool bOutputBlockedPin = false;
};

class FPCGExChannelGateElement : public IPCGElement
{
protected:
	virtual bool ExecuteInternal(FPCGContext* Context) const override;
	virtual bool IsCacheable(const UPCGSettings* InSettings) const override { return false; }
	virtual bool SupportsBasePointDataInputs(FPCGContext* InContext) const override { return true; }
};

/**
 * Routes input data to one output pin per selected channel, based on which channels are
 * active around the executing component. Unmatched data goes to the 'Default' pin.
 */
UCLASS(BlueprintType, ClassGroup = (Procedural), meta = (Keywords = "channel switch branch route layer"))
class PCGEXSCHEDULINGPOLICIES_API UPCGExChannelSwitchSettings : public UPCGExChannelNodeSettings
{
	GENERATED_BODY()

public:
	//~ Begin UPCGSettings interface
#if WITH_EDITOR
	virtual FName GetDefaultNodeName() const override { return FName(TEXT("ChannelSwitch")); }
	virtual FText GetDefaultNodeTitle() const override;
	virtual FText GetNodeTooltipText() const override;
	virtual EPCGSettingsType GetType() const override { return EPCGSettingsType::ControlFlow; }
#endif
	virtual FString GetAdditionalTitleInformation() const override;
	virtual bool HasDynamicPins() const override { return true; }
	virtual bool OutputPinsCanBeDeactivated() const override { return true; }

protected:
#if WITH_EDITOR
	virtual EPCGChangeType GetChangeTypeForProperty(const FName& InPropertyName) const override;
#endif
	virtual TArray<FPCGPinProperties> InputPinProperties() const override;
	virtual TArray<FPCGPinProperties> OutputPinProperties() const override;
	virtual FPCGElementPtr CreateElement() const override;
	//~ End UPCGSettings interface

public:
	/** One output pin is created per selected channel, in order. */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = Settings)
	FPCGExChannelSelector Channels;

	/** Route to every matching channel pin instead of only the first active one. */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = Settings)
	bool bRouteToAllMatching = false;

	/** Output pin labels: unique selected channels, then 'Default'. */
	void GetOutputPinLabels(TArray<FName>& OutLabels) const;
};

class FPCGExChannelSwitchElement : public IPCGElement
{
protected:
	virtual bool ExecuteInternal(FPCGContext* Context) const override;
	virtual bool IsCacheable(const UPCGSettings* InSettings) const override { return false; }
	virtual bool SupportsBasePointDataInputs(FPCGContext* InContext) const override { return true; }
};

/**
 * Outputs an attribute set with one 'Channel' (Name) entry per channel active around the
 * executing component — for custom graph-side logic.
 */
UCLASS(BlueprintType, ClassGroup = (Procedural), meta = (Keywords = "channel active get query layer"))
class PCGEXSCHEDULINGPOLICIES_API UPCGExGetActiveChannelsSettings : public UPCGExChannelNodeSettings
{
	GENERATED_BODY()

public:
	//~ Begin UPCGSettings interface
#if WITH_EDITOR
	virtual FName GetDefaultNodeName() const override { return FName(TEXT("GetActiveChannels")); }
	virtual FText GetDefaultNodeTitle() const override;
	virtual FText GetNodeTooltipText() const override;
	virtual EPCGSettingsType GetType() const override { return EPCGSettingsType::Param; }
#endif

protected:
	virtual TArray<FPCGPinProperties> InputPinProperties() const override;
	virtual TArray<FPCGPinProperties> OutputPinProperties() const override;
	virtual FPCGElementPtr CreateElement() const override;
	virtual bool HasExecutionDependencyPin() const override { return true; }
	//~ End UPCGSettings interface
};

class FPCGExGetActiveChannelsElement : public IPCGElement
{
protected:
	virtual bool ExecuteInternal(FPCGContext* Context) const override;
	virtual bool IsCacheable(const UPCGSettings* InSettings) const override { return false; }
};
