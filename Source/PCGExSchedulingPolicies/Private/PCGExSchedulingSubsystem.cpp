// Copyright 2026 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#include "PCGExSchedulingSubsystem.h"

#include "PCGExGenSourceComponent.h"
#include "PCGExSchedulingSettings.h"

#include "PCGCommon.h"
#include "PCGGraphExecutionStateInterface.h"
#include "PCGSubsystem.h"
#include "RuntimeGen/GenSources/PCGGenSourceBase.h"
#include "RuntimeGen/GenSources/PCGGenSourceComponent.h"
#include "RuntimeGen/GenSources/PCGGenSourcePlayer.h"

#include "EngineUtils.h"
#include "Components/SplineComponent.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/Volume.h"
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
	{
		FReadScopeLock ReadLock(SourceMasksLock);
		if (!SourceMasks.IsEmpty())
		{
			return true;
		}
	}

	FReadScopeLock ReadLock(TargetCachesLock);
	return !TargetCaches.IsEmpty();
}

TStatId UPCGExSchedulingSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UPCGExSchedulingSubsystem, STATGROUP_Tickables);
}

void UPCGExSchedulingSubsystem::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	const double Now = FPlatformTime::Seconds();
	const UPCGExSchedulingSettings* Settings = GetDefault<UPCGExSchedulingSettings>();

	if (Now - LastMaintenanceTime < Settings->SourceMaskRefreshInterval)
	{
		return;
	}

	LastMaintenanceTime = Now;

	TickSourceMaskMaintenance(Now);
	TickTargetCacheMaintenance(Now);
}

void UPCGExSchedulingSubsystem::TickSourceMaskMaintenance(const double InNow)
{
	// Periodic maintenance: drop dead sources, re-resolve live ones so runtime tag
	// changes (or pawn possession changes) are picked up without manual invalidation.
	TArray<FObjectKey> Keys;
	{
		FReadScopeLock ReadLock(SourceMasksLock);
		SourceMasks.GetKeys(Keys);
	}

	const uint32 Revision = GetDefault<UPCGExSchedulingSettings>()->GetRevision();

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

TSharedPtr<const FPCGExTargetSnapshot> UPCGExSchedulingSubsystem::ResolveTargetSnapshot(const UObject* InKey, TFunctionRef<FPCGExTargetQuery()> InQueryProvider, const UObject* InConsumer)
{
	check(IsInGameThread());

	if (!InKey)
	{
		return nullptr;
	}

	const FObjectKey Key(InKey);

	// Consumer = nearest execution source in the consumer's outer chain (typically the owning PCG component).
	FObjectKey ConsumerKey;
	for (const UObject* Walker = InConsumer; Walker; Walker = Walker->GetOuter())
	{
		if (Cast<IPCGGraphExecutionSource>(Walker))
		{
			ConsumerKey = FObjectKey(Walker);
			break;
		}
	}

	// Fast path: cached snapshot, consumer already registered.
	{
		FReadScopeLock ReadLock(TargetCachesLock);
		if (const FTargetCacheEntry* Entry = TargetCaches.Find(Key))
		{
			if (Entry->Snapshot && (ConsumerKey == FObjectKey() || Entry->Consumers.Contains(ConsumerKey)))
			{
				return Entry->Snapshot;
			}
		}
	}

	// Consumer-registration-only path: snapshot exists, consumer is new.
	{
		FWriteScopeLock WriteLock(TargetCachesLock);
		if (FTargetCacheEntry* Entry = TargetCaches.Find(Key))
		{
			if (Entry->Snapshot)
			{
				if (ConsumerKey != FObjectKey())
				{
					Entry->Consumers.Add(ConsumerKey);
				}
				return Entry->Snapshot;
			}
		}
	}

	// Build path.
	const FPCGExTargetQuery Query = InQueryProvider();

	TArray<TWeakObjectPtr<const AActor>> TagActors;
	if (!Query.TargetTag.IsNone())
	{
		TagActors = ScanTagActors(Query.TargetTag);
	}

	TMap<FObjectKey, FTransform> TrackedTransforms;
	TSharedPtr<const FPCGExTargetSnapshot> Snapshot = BuildTargetSnapshot(Query, TagActors, TrackedTransforms);

	{
		FWriteScopeLock WriteLock(TargetCachesLock);
		FTargetCacheEntry& Entry = TargetCaches.FindOrAdd(Key);
		Entry.Query = Query;
		Entry.Snapshot = Snapshot;
		Entry.TrackedTransforms = MoveTemp(TrackedTransforms);
		Entry.TagActors = MoveTemp(TagActors);
		Entry.LastTagScanTime = FPlatformTime::Seconds();
		if (ConsumerKey != FObjectKey())
		{
			Entry.Consumers.Add(ConsumerKey);
		}
	}

	return Snapshot;
}

TSharedPtr<const FPCGExTargetSnapshot> UPCGExSchedulingSubsystem::GetCachedTargetSnapshot(const UObject* InKey) const
{
	if (!InKey)
	{
		return nullptr;
	}

	FReadScopeLock ReadLock(TargetCachesLock);
	if (const FTargetCacheEntry* Entry = TargetCaches.Find(FObjectKey(InKey)))
	{
		// Possibly one maintenance-interval stale — by design, see class comment.
		return Entry->Snapshot;
	}

	return nullptr;
}

void UPCGExSchedulingSubsystem::InvalidateTargetCache(const UObject* InKey)
{
	if (!InKey)
	{
		return;
	}

	FWriteScopeLock WriteLock(TargetCachesLock);
	TargetCaches.Remove(FObjectKey(InKey));
}

void UPCGExSchedulingSubsystem::TickTargetCacheMaintenance(const double InNow)
{
	// Snapshot the work list under a brief lock, do all actor access unlocked, write results back.
	struct FWorkItem
	{
		FObjectKey Key;
		FPCGExTargetQuery Query;
		TArray<TWeakObjectPtr<const AActor>> TagActors;
		TMap<FObjectKey, FTransform> TrackedTransforms;
		TSet<FObjectKey> Consumers;
		double LastTagScanTime = 0.0;
	};

	TArray<FWorkItem> WorkItems;
	TArray<FObjectKey> DeadKeys;

	{
		FReadScopeLock ReadLock(TargetCachesLock);
		WorkItems.Reserve(TargetCaches.Num());

		for (const TPair<FObjectKey, FTargetCacheEntry>& Pair : TargetCaches)
		{
			if (!IsValid(Pair.Key.ResolveObjectPtr()))
			{
				DeadKeys.Add(Pair.Key);
				continue;
			}

			if (!Pair.Value.Snapshot)
			{
				continue;
			}

			WorkItems.Add({Pair.Key, Pair.Value.Query, Pair.Value.TagActors, Pair.Value.TrackedTransforms, Pair.Value.Consumers, Pair.Value.LastTagScanTime});
		}
	}

	if (!DeadKeys.IsEmpty())
	{
		FWriteScopeLock WriteLock(TargetCachesLock);
		for (const FObjectKey& DeadKey : DeadKeys)
		{
			TargetCaches.Remove(DeadKey);
		}
	}

	for (FWorkItem& Item : WorkItems)
	{
		bool bRebuild = false;

		// Amortized tag rescan.
		if (!Item.Query.TargetTag.IsNone() && InNow - Item.LastTagScanTime >= Item.Query.TagQueryInterval)
		{
			TArray<TWeakObjectPtr<const AActor>> Found = ScanTagActors(Item.Query.TargetTag);

			// Order-insensitive set comparison.
			TSet<const AActor*> Previous;
			for (const TWeakObjectPtr<const AActor>& Weak : Item.TagActors)
			{
				if (const AActor* Actor = Weak.Get()) { Previous.Add(Actor); }
			}

			TSet<const AActor*> Current;
			for (const TWeakObjectPtr<const AActor>& Weak : Found)
			{
				if (const AActor* Actor = Weak.Get()) { Current.Add(Actor); }
			}

			bRebuild = Previous.Num() != Current.Num() || !Previous.Includes(Current);

			Item.TagActors = MoveTemp(Found);
			Item.LastTagScanTime = InNow;
		}

		// Movement / lifetime tracking.
		if (!bRebuild && Item.Query.bTrackMovement && HaveTargetsMoved(Item.Query, Item.TrackedTransforms))
		{
			bRebuild = true;
		}

		if (!bRebuild)
		{
			// Persist the (possibly refreshed) tag bookkeeping.
			FWriteScopeLock WriteLock(TargetCachesLock);
			if (FTargetCacheEntry* Entry = TargetCaches.Find(Item.Key))
			{
				Entry->TagActors = Item.TagActors;
				Entry->LastTagScanTime = Item.LastTagScanTime;
			}
			continue;
		}

		TMap<FObjectKey, FTransform> TrackedTransforms;
		TSharedPtr<const FPCGExTargetSnapshot> Snapshot = BuildTargetSnapshot(Item.Query, Item.TagActors, TrackedTransforms);

		{
			FWriteScopeLock WriteLock(TargetCachesLock);
			if (FTargetCacheEntry* Entry = TargetCaches.Find(Item.Key))
			{
				Entry->Snapshot = Snapshot;
				Entry->TrackedTransforms = MoveTemp(TrackedTransforms);
				Entry->TagActors = Item.TagActors;
				Entry->LastTagScanTime = Item.LastTagScanTime;
			}
		}

		// Runtime-gen change detection only reacts to gen source movement — force a
		// cleanup+rescan so region changes are picked up while sources sit still.
		RefreshTargetConsumers(Item.Consumers);
	}
}

TArray<TWeakObjectPtr<const AActor>> UPCGExSchedulingSubsystem::ScanTagActors(const FName InTag) const
{
	check(IsInGameThread());

	TArray<TWeakObjectPtr<const AActor>> Found;
	if (InTag.IsNone())
	{
		return Found;
	}

	// O(world actors) — amortized by the per-constraint query interval.
	for (TActorIterator<AActor> It(GetWorld()); It; ++It)
	{
		if (It->ActorHasTag(InTag))
		{
			Found.Add(*It);
		}
	}

	return Found;
}

TSharedPtr<const FPCGExTargetSnapshot> UPCGExSchedulingSubsystem::BuildTargetSnapshot(const FPCGExTargetQuery& InQuery, const TArray<TWeakObjectPtr<const AActor>>& InTagActors, TMap<FObjectKey, FTransform>& OutTrackedTransforms) const
{
	check(IsInGameThread());

	OutTrackedTransforms.Reset();

	// Deduplicated target set: explicit references (when loaded — never force-loads) + tag scan results.
	TSet<const AActor*> Actors;
	const UWorld* World = GetWorld();

	for (const FSoftObjectPath& Path : InQuery.TargetActors)
	{
		const AActor* Actor = Cast<AActor>(Path.ResolveObject());
		if (IsValid(Actor) && Actor->GetWorld() == World)
		{
			Actors.Add(Actor);
		}
	}

	for (const TWeakObjectPtr<const AActor>& Weak : InTagActors)
	{
		if (const AActor* Actor = Weak.Get())
		{
			Actors.Add(Actor);
		}
	}

	TSharedRef<FPCGExTargetSnapshot> Snapshot = MakeShared<FPCGExTargetSnapshot>();
	Snapshot->Shapes.Reserve(Actors.Num());

	for (const AActor* Actor : Actors)
	{
		OutTrackedTransforms.Add(FObjectKey(Actor), Actor->GetActorTransform());

		if (InQuery.Geometry == EPCGExTargetGeometry::Spline)
		{
			// One shape per spline component; actors without usable splines fall back to a bounds shape.
			TInlineComponentArray<USplineComponent*> Splines(Actor);
			bool bAnySpline = false;

			for (const USplineComponent* Spline : Splines)
			{
				if (!Spline)
				{
					continue;
				}

				FPCGExTargetShape Shape;
				if (Spline->ConvertSplineToPolyLine(ESplineCoordinateSpace::World, FMath::Square(InQuery.SplineErrorTolerance), Shape.SplinePoints) && Shape.SplinePoints.Num() >= 2)
				{
					Shape.bClosedSpline = Spline->IsClosedLoop();
					Shape.Bounds = FBox(Shape.SplinePoints);
					Snapshot->Shapes.Add(MoveTemp(Shape));
					bAnySpline = true;
				}
			}

			if (bAnySpline)
			{
				continue;
			}
		}

		FPCGExTargetShape& Shape = Snapshot->Shapes.Emplace_GetRef();
		Shape.Bounds = Actor->GetComponentsBoundingBox(/*bNonColliding=*/true);

		if (InQuery.Geometry == EPCGExTargetGeometry::Volume)
		{
			Shape.Volume = Cast<AVolume>(Actor);
		}
	}

	return Snapshot;
}

bool UPCGExSchedulingSubsystem::HaveTargetsMoved(const FPCGExTargetQuery& InQuery, const TMap<FObjectKey, FTransform>& InTrackedTransforms) const
{
	check(IsInGameThread());

	for (const TPair<FObjectKey, FTransform>& Pair : InTrackedTransforms)
	{
		const AActor* Actor = Cast<AActor>(Pair.Key.ResolveObjectPtr());
		if (!IsValid(Actor))
		{
			// Target died — rebuild to drop it.
			return true;
		}

		const FTransform Current = Actor->GetActorTransform();
		if (!Current.GetLocation().Equals(Pair.Value.GetLocation(), InQuery.MovementTolerance)
			|| !Current.GetRotation().Equals(Pair.Value.GetRotation(), 1.e-3)
			|| !Current.GetScale3D().Equals(Pair.Value.GetScale3D(), 1.e-2))
		{
			return true;
		}
	}

	return false;
}

void UPCGExSchedulingSubsystem::RefreshTargetConsumers(const TSet<FObjectKey>& InConsumers) const
{
	check(IsInGameThread());

	UPCGSubsystem* PCGSubsystem = UPCGSubsystem::GetInstance(GetWorld());
	if (!PCGSubsystem)
	{
		return;
	}

	for (const FObjectKey& ConsumerKey : InConsumers)
	{
		UObject* Consumer = ConsumerKey.ResolveObjectPtr();
		if (!IsValid(Consumer))
		{
			continue;
		}

		if (IPCGGraphExecutionSource* ExecutionSource = Cast<IPCGGraphExecutionSource>(Consumer))
		{
			PCGSubsystem->RefreshRuntimeGenExecutionSource(ExecutionSource, EPCGChangeType::None);
		}
	}
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
