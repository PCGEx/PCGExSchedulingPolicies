// Copyright 2026 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#include "PCGExSchedulingSubsystem.h"

#include "PCGExChannelProvider.h"
#include "PCGExSchedulingSettings.h"

#include "PCGCommon.h"
#include "PCGGraphExecutionStateInterface.h"
#include "PCGSubsystem.h"
#include "Helpers/PCGHelpers.h"
#include "RuntimeGen/PCGGenSourceManager.h"
#include "RuntimeGen/GenSources/PCGGenSourceBase.h"
#include "RuntimeGen/GenSources/PCGGenSourceComponent.h"
#include "RuntimeGen/GenSources/PCGGenSourceEditorCamera.h"
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
	// Always tick: the active-sources snapshot must exist before the first policy or
	// node ever touches the subsystem. The per-frame body early-outs cheaply.
	return true;
}

TStatId UPCGExSchedulingSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UPCGExSchedulingSubsystem, STATGROUP_Tickables);
}

void UPCGExSchedulingSubsystem::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	// Per-frame: keep the active-sources snapshot fresh for graph nodes (positions move every frame).
	RebuildActiveSourcesSnapshot();

	const double Now = FPlatformTime::Seconds();
	const UPCGExSchedulingSettings* Settings = GetDefault<UPCGExSchedulingSettings>();

	if (Now - LastMaintenanceTime < Settings->SourceMaskRefreshInterval)
	{
		return;
	}

	LastMaintenanceTime = Now;

	TickSourceMaskMaintenance();
	TickTargetCacheMaintenance(Now);
}

void UPCGExSchedulingSubsystem::RebuildActiveSourcesSnapshot()
{
	TSharedPtr<const FPCGExActiveSourcesSnapshot> NewSnapshot;

	if (UPCGSubsystem* PCGSubsystem = UPCGSubsystem::GetInstance(GetWorld()))
	{
		FPCGGenSourceManager* GenSourceManager = PCGSubsystem->GetGenSourceManager();

		// Find-only: GetPCGWorldActor() lazily SPAWNS a transactional world actor into
		// editor worlds -- a PCG-free map must not grow one just because this plugin ticks.
		// When PCG is actually in use, the engine has already created/registered the actor.
		const APCGWorldActor* PCGWorldActor = PCGSubsystem->FindPCGWorldActor();

		if (GenSourceManager && PCGWorldActor)
		{
			// Channel table is revision-stable -- share one immutable copy across frames.
			const UPCGExSchedulingSettings* Settings = GetDefault<UPCGExSchedulingSettings>();
			if (!CachedChannelTable || CachedChannelTableRevision != Settings->GetRevision())
			{
				TSharedRef<FPCGExActiveSourcesSnapshot::FChannelTable> Table = MakeShared<FPCGExActiveSourcesSnapshot::FChannelTable>();
				Settings->GetChannelTable(*Table);
				CachedChannelTable = Table;
				CachedChannelTableRevision = Settings->GetRevision();
			}

			TSharedRef<FPCGExActiveSourcesSnapshot> Building = MakeShared<FPCGExActiveSourcesSnapshot>();
			Building->ChannelTable = CachedChannelTable;

			// Benign wrt the scheduler: sources are ticked at most once per dirty cycle,
			// whether we or the runtime-gen scheduler consume the dirty flag first.
			const TSet<IPCGGenSourceBase*> GenSources = GenSourceManager->GetAllGenSources(PCGWorldActor);
			Building->Sources.Reserve(GenSources.Num());

			for (IPCGGenSourceBase* GenSource : GenSources)
			{
				if (!GenSource)
				{
					continue;
				}

				const TOptional<FVector> Position = GenSource->GetPosition();
				if (!Position.IsSet())
				{
					continue;
				}

				FPCGExActiveSourcesSnapshot::FSourceState& State = Building->Sources.Emplace_GetRef();
				State.Position = Position.GetValue();
				State.Mask = ResolveSourceMask(GenSource);
				State.bIsEditorCamera = IsEditorCameraSource(GenSource);
			}

			NewSnapshot = Building;
		}
	}

	// Publish only on change -- spares worker readers the per-frame write barrier and refcount churn.
	{
		FReadScopeLock ReadLock(ActiveSourcesLock);

		const bool bBothNull = !NewSnapshot && !ActiveSourcesSnapshot;
		const bool bUnchanged = NewSnapshot && ActiveSourcesSnapshot
			&& NewSnapshot->ChannelTable == ActiveSourcesSnapshot->ChannelTable
			&& NewSnapshot->Sources == ActiveSourcesSnapshot->Sources;

		if (bBothNull || bUnchanged)
		{
			return;
		}
	}

	FWriteScopeLock WriteLock(ActiveSourcesLock);
	ActiveSourcesSnapshot = MoveTemp(NewSnapshot);
}

bool UPCGExSchedulingSubsystem::IsEditorCameraSource(const IPCGGenSourceBase* InGenSource)
{
	return Cast<UPCGGenSourceEditorCamera>(InGenSource) != nullptr;
}

TSharedPtr<const FPCGExActiveSourcesSnapshot> UPCGExSchedulingSubsystem::GetActiveSourcesSnapshot() const
{
	FReadScopeLock ReadLock(ActiveSourcesLock);
	return ActiveSourcesSnapshot;
}

void UPCGExSchedulingSubsystem::TickSourceMaskMaintenance()
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
		// Possibly one tick stale -- by design, see class comment.
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
	TArray<FSoftObjectPath> UnresolvedTargets;
	TSharedPtr<const FPCGExTargetSnapshot> Snapshot = BuildTargetSnapshot(Query, TagActors, TrackedTransforms, UnresolvedTargets);

	{
		FWriteScopeLock WriteLock(TargetCachesLock);
		FTargetCacheEntry& Entry = TargetCaches.FindOrAdd(Key);
		Entry.Query = Query;
		Entry.Snapshot = Snapshot;
		Entry.TrackedTransforms = MoveTemp(TrackedTransforms);
		Entry.UnresolvedTargets = MoveTemp(UnresolvedTargets);
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
		// Possibly one maintenance-interval stale -- by design, see class comment.
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
	// Snapshot a SLIM work list under a brief lock (decision inputs only -- the full query
	// and consumer set are fetched lazily, only when a rebuild actually triggers), do all
	// actor access unlocked, write results back under short write locks.
	struct FWorkItem
	{
		FObjectKey Key;
		FName TargetTag = NAME_None;
		float TagQueryInterval = 2.0f;
		bool bTrackMovement = true;
		float MovementTolerance = 50.0f;
		double LastTagScanTime = 0.0;
		TArray<TWeakObjectPtr<const AActor>> TagActors;
		TMap<FObjectKey, FTransform> TrackedTransforms;
		TArray<FSoftObjectPath> UnresolvedTargets;
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

			FWorkItem& Item = WorkItems.Emplace_GetRef();
			Item.Key = Pair.Key;
			Item.TargetTag = Pair.Value.Query.TargetTag;
			Item.TagQueryInterval = Pair.Value.Query.TagQueryInterval;
			Item.bTrackMovement = Pair.Value.Query.bTrackMovement;
			Item.MovementTolerance = Pair.Value.Query.MovementTolerance;
			Item.LastTagScanTime = Pair.Value.LastTagScanTime;
			Item.UnresolvedTargets = Pair.Value.UnresolvedTargets;

			if (!Item.TargetTag.IsNone())
			{
				Item.TagActors = Pair.Value.TagActors;
			}

			if (Item.bTrackMovement)
			{
				Item.TrackedTransforms = Pair.Value.TrackedTransforms;
			}
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
		if (!Item.TargetTag.IsNone() && InNow - Item.LastTagScanTime >= Item.TagQueryInterval)
		{
			TArray<TWeakObjectPtr<const AActor>> Found = ScanTagActors(Item.TargetTag);

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

		// Previously-unloaded explicit references that streamed in since the last build.
		if (!bRebuild && HaveTargetsResolved(Item.UnresolvedTargets))
		{
			bRebuild = true;
		}

		// Movement / lifetime tracking.
		if (!bRebuild && Item.bTrackMovement && HaveTargetsMoved(Item.TrackedTransforms, Item.MovementTolerance))
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

		// Full inputs are only needed now that a rebuild is certain.
		FPCGExTargetQuery Query;
		TSet<FObjectKey> Consumers;
		{
			FReadScopeLock ReadLock(TargetCachesLock);
			const FTargetCacheEntry* Entry = TargetCaches.Find(Item.Key);
			if (!Entry)
			{
				continue;
			}
			Query = Entry->Query;
			Consumers = Entry->Consumers;
		}

		TMap<FObjectKey, FTransform> TrackedTransforms;
		TArray<FSoftObjectPath> UnresolvedTargets;
		TSharedPtr<const FPCGExTargetSnapshot> Snapshot = BuildTargetSnapshot(Query, Item.TagActors, TrackedTransforms, UnresolvedTargets);

		{
			FWriteScopeLock WriteLock(TargetCachesLock);
			if (FTargetCacheEntry* Entry = TargetCaches.Find(Item.Key))
			{
				Entry->Snapshot = Snapshot;
				Entry->TrackedTransforms = MoveTemp(TrackedTransforms);
				Entry->UnresolvedTargets = MoveTemp(UnresolvedTargets);
				Entry->TagActors = Item.TagActors;
				Entry->LastTagScanTime = Item.LastTagScanTime;
			}
		}

		// Runtime-gen change detection only reacts to gen source movement -- force a
		// cleanup+rescan so region changes are picked up while sources sit still.
		RefreshTargetConsumers(Consumers);
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

	// O(world actors) -- amortized by the per-constraint query interval.
	for (TActorIterator<AActor> It(GetWorld()); It; ++It)
	{
		if (It->ActorHasTag(InTag))
		{
			Found.Add(*It);
		}
	}

	return Found;
}

TSharedPtr<const FPCGExTargetSnapshot> UPCGExSchedulingSubsystem::BuildTargetSnapshot(const FPCGExTargetQuery& InQuery, const TArray<TWeakObjectPtr<const AActor>>& InTagActors, TMap<FObjectKey, FTransform>& OutTrackedTransforms, TArray<FSoftObjectPath>& OutUnresolvedTargets) const
{
	check(IsInGameThread());

	OutTrackedTransforms.Reset();
	OutUnresolvedTargets.Reset();

	// Deduplicated target set: explicit references (when loaded -- never force-loads) + tag scan results.
	// References that don't resolve are remembered so maintenance can pick them up when they stream in.
	TSet<const AActor*> Actors;
	const UWorld* World = GetWorld();

	for (const FSoftObjectPath& Path : InQuery.TargetActors)
	{
		const AActor* Actor = Cast<AActor>(Path.ResolveObject());
		if (IsValid(Actor) && Actor->GetWorld() == World)
		{
			Actors.Add(Actor);
		}
		else
		{
			OutUnresolvedTargets.Add(Path);
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
		// PCG-aware bounds: excludes PCG-generated components, so a target hosting PCG
		// output doesn't inflate its own region (self-reinforcing feedback loop).
		Shape.Bounds = PCGHelpers::GetActorBounds(Actor);

		if (InQuery.Geometry == EPCGExTargetGeometry::Volume)
		{
			Shape.Volume = Cast<AVolume>(Actor);
		}
	}

	return Snapshot;
}

bool UPCGExSchedulingSubsystem::HaveTargetsMoved(const TMap<FObjectKey, FTransform>& InTrackedTransforms, const double InMovementTolerance) const
{
	check(IsInGameThread());

	for (const TPair<FObjectKey, FTransform>& Pair : InTrackedTransforms)
	{
		const AActor* Actor = Cast<AActor>(Pair.Key.ResolveObjectPtr());
		if (!IsValid(Actor))
		{
			// Target died -- rebuild to drop it.
			return true;
		}

		const FTransform Current = Actor->GetActorTransform();
		if (!Current.GetLocation().Equals(Pair.Value.GetLocation(), InMovementTolerance)
			|| !Current.GetRotation().Equals(Pair.Value.GetRotation(), 1.e-3)
			|| !Current.GetScale3D().Equals(Pair.Value.GetScale3D(), 1.e-2))
		{
			return true;
		}
	}

	return false;
}

bool UPCGExSchedulingSubsystem::HaveTargetsResolved(const TArray<FSoftObjectPath>& InUnresolvedTargets) const
{
	check(IsInGameThread());

	const UWorld* World = GetWorld();

	for (const FSoftObjectPath& Path : InUnresolvedTargets)
	{
		const AActor* Actor = Cast<AActor>(Path.ResolveObject());
		if (IsValid(Actor) && Actor->GetWorld() == World)
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
	check(IsInGameThread());

	const UPCGExSchedulingSettings* Settings = GetDefault<UPCGExSchedulingSettings>();
	const UObject* SourceObject = PCGExSchedulingSubsystem::AsObject(InGenSource);

	// Channel providers (our component, or any third-party gen source opting in): explicit channels, no tag mixing.
	if (const IPCGExChannelProvider* Provider = Cast<IPCGExChannelProvider>(SourceObject))
	{
		TArray<FName> ProviderChannels;
		Provider->GetSchedulingChannels(ProviderChannels);
		return Settings->ResolveChannelNames(ProviderChannels);
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
