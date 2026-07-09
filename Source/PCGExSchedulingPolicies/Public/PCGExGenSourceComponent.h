// Copyright 2026 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "RuntimeGen/GenSources/PCGGenSourceBase.h"

#include "PCGExChannelProvider.h"
#include "PCGExSchedulingCommon.h"

#include "PCGExGenSourceComponent.generated.h"

namespace EEndPlayReason { enum Type : int; }

class FPCGGenSourceManager;

/**
 * PCG runtime generation source with PCGEx scheduling channels: channel-filtered
 * PCGEx scheduling policies only react to sources whose channels overlap their own.
 *
 * Implements IPCGGenSourceBase directly (mirroring UPCGGenSourceComponent's registration
 * lifecycle -- the engine component is not subclassable, its constructor is private) so it
 * is a drop-in replacement: position is the owner's location, direction the owner's forward.
 */
UCLASS(BlueprintType, ClassGroup = (Procedural), DisplayName = "PCGEx Generation Source", meta = (BlueprintSpawnableComponent, PrioritizeCategories = "PCG"))
class PCGEXSCHEDULINGPOLICIES_API UPCGExGenSourceComponent : public UActorComponent, public IPCGGenSourceBase, public IPCGExChannelProvider
{
	GENERATED_BODY()

public:
	virtual ~UPCGExGenSourceComponent() override;

	//~ Begin UActorComponent interface
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void OnRegister() override;
	virtual void OnUnregister() override;
	virtual void OnComponentCreated() override;
	virtual void OnComponentDestroyed(bool bDestroyingHierarchy) override;
#if WITH_EDITOR
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
#endif
	//~ End UActorComponent interface

	//~ Begin IPCGGenSourceBase interface
	virtual TOptional<FVector> GetPosition() const override;
	virtual TOptional<FVector> GetDirection() const override;
	virtual bool IsEquivalent(IPCGGenSourceBase* InOther, const FPCGRuntimeGenContext& InContext) const override;
	virtual FString GetDebugName() const override { return GetName(); }
	//~ End IPCGGenSourceBase interface

	//~ Begin IPCGExChannelProvider interface
	virtual void GetSchedulingChannels(TArray<FName>& OutChannels) const override { OutChannels = Channels.Channels; }
	//~ End IPCGExChannelProvider interface

	/** Channels this source broadcasts on. Channel-filtered policies only react when their selection overlaps. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "PCG")
	FPCGExChannelSelector Channels;

	/** Sets the broadcast channels and invalidates the cached channel mask. */
	UFUNCTION(BlueprintCallable, Category = "PCG")
	void SetChannels(const FPCGExChannelSelector& InChannels);

	/** Resolved channel mask. Game thread (resolves through the scheduling subsystem). */
	PCGExScheduling::FChannelMask GetChannelMask() const;

protected:
	FPCGGenSourceManager* GetGenSourceManager() const;
	void RegisterGenSource();
	void UnregisterGenSource();
	void InvalidateCachedMask() const;
};
