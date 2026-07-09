// Copyright 2026 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#include "PCGExSchedulingSubsystem.h"

#include "PCGExGenSourceComponent.h"
#include "PCGExSchedulingSettings.h"

#include "RuntimeGen/GenSources/PCGGenSourceBase.h"
#include "RuntimeGen/GenSources/PCGGenSourceComponent.h"
#include "RuntimeGen/GenSources/PCGGenSourcePlayer.h"

#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "HAL/PlatformTime.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(PCGExSchedulingSubsystem)

namespace PCGExSchedulingSubsystem
{
	FORCEINLINE const UObject* AsObject(const IPCGGenSourceBase* InGenSource)
	{
		// Generation sources are always UObjects (the manager stores TScriptInterface).
		return Cast<UObject>(const_cast<IPCGGenSourceBase*>(InGenSource));
	}
}

UPCGExSchedulingSubsystem* UPCGExSchedulingSubsystem::GetInstance(const UWorld* InWorld)
{
	return InWorld ? InWorld->GetSubsystem<UPCGExSchedulingSubsystem>() : nullptr;
}

bool UPCGExSchedulingSubsystem::IsTickable() const
{
	FReadScopeLock ReadLock(SourceMasksLock);
	return !SourceMasks.IsEmpty();
}

TStatId UPCGExSchedulingSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UPCGExSchedulingSubsystem, STATGROUP_Tickables);
}

void UPCGExSchedulingSubsystem::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	// Periodic maintenance: drop dead sources, re-resolve live ones so runtime tag
	// changes (or pawn possession changes) are picked up without manual invalidation.
	const double Now = FPlatformTime::Seconds();
	const UPCGExSchedulingSettings* Settings = GetDefault<UPCGExSchedulingSettings>();

	if (Now - LastMaintenanceTime < Settings->SourceMaskRefreshInterval)
	{
		return;
	}

	LastMaintenanceTime = Now;

	TArray<FObjectKey> Keys;
	{
		FReadScopeLock ReadLock(SourceMasksLock);
		SourceMasks.GetKeys(Keys);
	}

	const uint32 Revision = Settings->GetRevision();

	TArray<TPair<FObjectKey, FResolvedSource>> Updates;
	TArray<FObjectKey> DeadKeys;
	Updates.Reserve(Keys.Num());

	for (const FObjectKey& Key : Keys)
	{
		const UObject* Object = Key.ResolveObjectPtr();
		const IPCGGenSourceBase* GenSource = IsValid(Object) ? Cast<IPCGGenSourceBase>(Object) : nullptr;

		if (!GenSource)
		{
			DeadKeys.Add(Key);
			continue;
		}

		Updates.Emplace(Key, FResolvedSource{ResolveMaskInternal(GenSource), Revision});
	}

	{
		FWriteScopeLock WriteLock(SourceMasksLock);
		for (const FObjectKey& DeadKey : DeadKeys) { SourceMasks.Remove(DeadKey); }
		for (const TPair<FObjectKey, FResolvedSource>& Update : Updates) { SourceMasks.Add(Update.Key, Update.Value); }
	}
}

PCGExScheduling::FChannelMask UPCGExSchedulingSubsystem::ResolveSourceMask(const IPCGGenSourceBase* InGenSource)
{
	check(IsInGameThread());

	const UObject* SourceObject = PCGExSchedulingSubsystem::AsObject(InGenSource);
	if (!SourceObject)
	{
		return 0;
	}

	const FObjectKey Key(SourceObject);
	const uint32 Revision = GetDefault<UPCGExSchedulingSettings>()->GetRevision();

	{
		FReadScopeLock ReadLock(SourceMasksLock);
		if (const FResolvedSource* Found = SourceMasks.Find(Key))
		{
			if (Found->Revision == Revision)
			{
				return Found->Mask;
			}
		}
	}

	const PCGExScheduling::FChannelMask Mask = ResolveMaskInternal(InGenSource);

	{
		FWriteScopeLock WriteLock(SourceMasksLock);
		SourceMasks.Add(Key, FResolvedSource{Mask, Revision});
	}

	return Mask;
}

TOptional<PCGExScheduling::FChannelMask> UPCGExSchedulingSubsystem::GetCachedSourceMask(const IPCGGenSourceBase* InGenSource) const
{
	const UObject* SourceObject = PCGExSchedulingSubsystem::AsObject(InGenSource);
	if (!SourceObject)
	{
		return TOptional<PCGExScheduling::FChannelMask>();
	}

	FReadScopeLock ReadLock(SourceMasksLock);
	if (const FResolvedSource* Found = SourceMasks.Find(FObjectKey(SourceObject)))
	{
		// Possibly one tick stale — by design, see class comment.
		return Found->Mask;
	}

	return TOptional<PCGExScheduling::FChannelMask>();
}

TOptional<PCGExScheduling::FChannelMask> UPCGExSchedulingSubsystem::GetOrResolveSourceMask(const IPCGGenSourceBase* InGenSource)
{
	if (IsInGameThread())
	{
		return ResolveSourceMask(InGenSource);
	}

	return GetCachedSourceMask(InGenSource);
}

void UPCGExSchedulingSubsystem::InvalidateSource(const UObject* InSourceObject)
{
	if (!InSourceObject)
	{
		return;
	}

	FWriteScopeLock WriteLock(SourceMasksLock);
	SourceMasks.Remove(FObjectKey(InSourceObject));
}

PCGExScheduling::FChannelMask UPCGExSchedulingSubsystem::ResolveMaskInternal(const IPCGGenSourceBase* InGenSource) const
{
	const UPCGExSchedulingSettings* Settings = GetDefault<UPCGExSchedulingSettings>();
	const UObject* SourceObject = PCGExSchedulingSubsystem::AsObject(InGenSource);

	// PCGEx component: explicit channels only — predictable authoring, no tag mixing.
	if (const UPCGExGenSourceComponent* ExComponent = Cast<UPCGExGenSourceComponent>(SourceObject))
	{
		return Settings->ResolveChannelNames(ExComponent->Channels.Channels);
	}

	// Vanilla component: owner actor tags + component tags through the settings tag map.
	if (const UPCGGenSourceComponent* Component = Cast<UPCGGenSourceComponent>(SourceObject))
	{
		PCGExScheduling::FChannelMask Mask = Settings->ResolveTags(Component->ComponentTags);
		if (const AActor* Owner = Component->GetOwner())
		{
			Mask |= Settings->ResolveTags(Owner->Tags);
		}
		return Mask;
	}

	// Automatic player source: settings-assigned channels + controller/pawn tags.
	if (const UPCGGenSourcePlayer* Player = Cast<UPCGGenSourcePlayer>(SourceObject))
	{
		PCGExScheduling::FChannelMask Mask = Settings->ResolveChannelNames(Settings->PlayerChannels.Channels);
		if (const APlayerController* Controller = Player->GetPlayerController().Get())
		{
			Mask |= Settings->ResolveTags(Controller->Tags);
			if (const APawn* Pawn = Controller->GetPawn())
			{
				Mask |= Settings->ResolveTags(Pawn->Tags);
			}
		}
		return Mask;
	}

	// Editor camera and unknown external sources: no channels.
	// (The editor camera is handled by the policy-level bypass instead.)
	return 0;
}
