// Copyright 2026 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#include "PCGExGenSourceComponent.h"

#include "PCGExSchedulingSubsystem.h"

#include "PCGSubsystem.h"
#include "RuntimeGen/PCGGenSourceManager.h"

#include "Engine/World.h"
#include "GameFramework/Actor.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(PCGExGenSourceComponent)

UPCGExGenSourceComponent::~UPCGExGenSourceComponent()
{
#if WITH_EDITOR
	// For the special case where a component is part of a reconstruction script (from a BP),
	// but gets destroyed immediately, we need to force the unregistering.
	if (UPCGSubsystem* PCGSubsystem = UPCGSubsystem::GetSubsystemForCurrentWorld())
	{
		if (FPCGGenSourceManager* GenSourceManager = PCGSubsystem->GetGenSourceManager())
		{
			GenSourceManager->UnregisterGenSource(this);
		}
	}
#endif // WITH_EDITOR
}

void UPCGExGenSourceComponent::BeginPlay()
{
	Super::BeginPlay();

#if WITH_EDITOR
	// Disable registration on preview actors.
	if (GetOwner() && GetOwner()->bIsEditorPreviewActor)
	{
		return;
	}
#endif

	RegisterGenSource();
}

void UPCGExGenSourceComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	// Always try to unregister; if the source isn't registered, it will early out.
	UnregisterGenSource();

	Super::EndPlay(EndPlayReason);
}

void UPCGExGenSourceComponent::OnRegister()
{
	Super::OnRegister();

#if WITH_EDITOR
	// Disable registration on preview actors.
	if (GetOwner() && GetOwner()->bIsEditorPreviewActor)
	{
		return;
	}

	if (UWorld* World = GetWorld())
	{
		// We won't be able to spawn any actors if we are currently running a construction script.
		if (!World->bIsRunningConstructionScript)
		{
			InvalidateCachedMask();
			RegisterGenSource();
		}
	}
#endif // WITH_EDITOR
}

void UPCGExGenSourceComponent::OnUnregister()
{
	Super::OnUnregister();

#if WITH_EDITOR
	// Disable registration on preview actors.
	if (GetOwner() && GetOwner()->bIsEditorPreviewActor)
	{
		return;
	}

	UnregisterGenSource();
#endif // WITH_EDITOR
}

void UPCGExGenSourceComponent::OnComponentCreated()
{
	Super::OnComponentCreated();
	RegisterGenSource();
}

void UPCGExGenSourceComponent::OnComponentDestroyed(const bool bDestroyingHierarchy)
{
	Super::OnComponentDestroyed(bDestroyingHierarchy);
	UnregisterGenSource();
}

#if WITH_EDITOR
void UPCGExGenSourceComponent::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);
	InvalidateCachedMask();
}
#endif

TOptional<FVector> UPCGExGenSourceComponent::GetPosition() const
{
	if (const AActor* Actor = GetOwner())
	{
		return Actor->GetActorLocation();
	}

	return TOptional<FVector>();
}

TOptional<FVector> UPCGExGenSourceComponent::GetDirection() const
{
	if (const AActor* Actor = GetOwner())
	{
		return Actor->GetActorForwardVector();
	}

	return TOptional<FVector>();
}

bool UPCGExGenSourceComponent::IsEquivalent(IPCGGenSourceBase* InOther, const FPCGRuntimeGenContext& InContext) const
{
	if (!IPCGGenSourceBase::IsEquivalent(InOther, InContext))
	{
		return false;
	}

	// Sources broadcasting on different channel sets must never be collapsed by the
	// scheduler's scan dedup — a collapsed source is never presented to any policy.
	// Dedup runs on the game thread (FPCGGenSourceManager::GetUniqueGenSources).
	if (UPCGExSchedulingSubsystem* Subsystem = UPCGExSchedulingSubsystem::GetInstance(GetWorld()))
	{
		return Subsystem->ResolveSourceMask(this) == Subsystem->ResolveSourceMask(InOther);
	}

	return true;
}

void UPCGExGenSourceComponent::SetChannels(const FPCGExChannelSelector& InChannels)
{
	Channels = InChannels;
	InvalidateCachedMask();
}

PCGExScheduling::FChannelMask UPCGExGenSourceComponent::GetChannelMask() const
{
	if (UPCGExSchedulingSubsystem* Subsystem = UPCGExSchedulingSubsystem::GetInstance(GetWorld()))
	{
		return Subsystem->ResolveSourceMask(this);
	}

	return 0;
}

FPCGGenSourceManager* UPCGExGenSourceComponent::GetGenSourceManager() const
{
	if (const AActor* Actor = GetOwner())
	{
		if (UPCGSubsystem* Subsystem = UPCGSubsystem::GetInstance(Actor->GetWorld()))
		{
			return Subsystem->GetGenSourceManager();
		}
	}

	return nullptr;
}

void UPCGExGenSourceComponent::RegisterGenSource()
{
	if (FPCGGenSourceManager* GenSourceManager = GetGenSourceManager())
	{
		GenSourceManager->RegisterGenSource(this);
	}
}

void UPCGExGenSourceComponent::UnregisterGenSource()
{
	if (FPCGGenSourceManager* GenSourceManager = GetGenSourceManager())
	{
		GenSourceManager->UnregisterGenSource(this);
	}
}

void UPCGExGenSourceComponent::InvalidateCachedMask() const
{
	if (UPCGExSchedulingSubsystem* Subsystem = UPCGExSchedulingSubsystem::GetInstance(GetWorld()))
	{
		Subsystem->InvalidateSource(this);
	}
}
