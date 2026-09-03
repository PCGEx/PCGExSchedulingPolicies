// Copyright 2026 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#include "PCGExSchedulingSubsystem.h"

#include "PCGExChannelProvider.h"
#include "PCGExSchedulingPolicy.h"
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

#if WITH_EDITOR
#include "Editor.h"
#endif

#include "EngineDefines.h"
#include "EngineUtils.h"
#include "Components/SplineComponent.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/Volume.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformTime.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(PCGExSchedulingSubsystem)

namespace PCGExSchedulingSubsystem
{
#if UE_ENABLE_DEBUG_DRAWING
	TAutoConsoleVariable<int32> CVarDebugDraw(
		TEXT("pcgex.Scheduling.DebugDraw"),
		0,
		TEXT("Draws the PCGEx scheduling constraints of every active runtime-generated component (resolving their targets if needed). 0: off, 1: generate regions, 2: generate + cleanup regions."));
#endif

	FORCEINLINE const UObject* AsObject(const IPCGGenSourceBase* InGenSource)
	{
		// Generation sources are always UObjects (the manager stores TScriptInterface).
		return Cast<UObject>(const_cast<IPCGGenSourceBase*>(InGenSource));
	}

#if WITH_EDITOR
	bool IsViewportClientLive(const FEditorViewportClient* InClient)
	{
		// ~FEditorViewportClient unregisters itself from GEditor's client list, making membership
		// the only reliable liveness signal for the gen source manager's raw pointer (which nothing
		// nulls on destruction -- it is merely re-resolved whenever the runtime-gen scheduler
		// dirties the manager).
		return InClient && GEditor && GEditor->GetAllViewportClients().Contains(InClient);
	}
#endif

	/** Execution source consumer of a referencer: the nearest execution source in its outer chain (typically the owning PCG component). */
	FObjectKey FindConsumerKey(const UObject* InReferencer)
	{
		for (const UObject* Walker = InReferencer; Walker; Walker = Walker->GetOuter())
		{
			if (Cast<IPCGGraphExecutionSource>(Walker))
			{
				return FObjectKey(Walker);
			}
		}
		return FObjectKey();
	}
}

UPCGExSchedulingSubsystem* UPCGExSchedulingSubsystem::GetInstance(const UWorld* InWorld)
{
	return InWorld ? InWorld->GetSubsystem<UPCGExSchedulingSubsystem>() : nullptr;
}

void UPCGExSchedulingSubsystem::Deinitialize()
{
	// Consumers keep their slots past the world -- orphan them so they re-acquire from the next subsystem.
	for (TPair<FPCGExTargetQuery, FTargetCacheEntry>& Pair : TargetCaches)
	{
		if (Pair.Value.Slot)
		{
			Pair.Value.Slot->bOrphaned = true;
		}
	}
	TargetCaches.Empty();

	Super::Deinitialize();
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

	// Skip all world/PCG access once shutdown begins. bIsTearingDown covers map change and PIE end,
	// but on editor close the Slate windows (and their FEditorViewportClients) are destroyed in the
	// same stack that requests engine exit -- BEFORE any world starts tearing down -- so the exit
	// request is the only flag raised early enough.
	UWorld* World = GetWorld();
	if (IsEngineExitRequested() || !World || World->bIsTearingDown)
	{
		return;
	}

	// One source gather per frame, shared by the snapshot and the debug draw.
	UPCGSubsystem* PCGSubsystem = UPCGSubsystem::GetInstance(World);
	bool bHasRuntimeGen = false;
	GatherGenSources(PCGSubsystem, bHasRuntimeGen);

	// Per-frame: keep the active-sources snapshot fresh for graph nodes (positions move every frame).
	RebuildActiveSourcesSnapshot(bHasRuntimeGen);

	const double Now = FPlatformTime::Seconds();
	const UPCGExSchedulingSettings* Settings = GetDefault<UPCGExSchedulingSettings>();

	if (Now - LastMaintenanceTime >= Settings->SourceMaskRefreshInterval)
	{
		LastMaintenanceTime = Now;
		TickSourceMaskMaintenance();
	}

	if (Now >= NextTargetMaintenanceTime)
	{
		TickTargetCacheMaintenance(Now);
	}

#if UE_ENABLE_DEBUG_DRAWING
	const int32 DebugDrawMode = PCGExSchedulingSubsystem::CVarDebugDraw.GetValueOnGameThread();
	if (DebugDrawMode > 0 && PCGSubsystem && bHasRuntimeGen)
	{
		DebugDraw(PCGSubsystem, DebugDrawMode);
	}
#endif
}

void UPCGExSchedulingSubsystem::GatherGenSources(UPCGSubsystem* InPCGSubsystem, bool& bOutHasRuntimeGen)
{
	bOutHasRuntimeGen = false;
	ScratchGenSources.Reset();

	if (!InPCGSubsystem)
	{
		return;
	}

	FPCGGenSourceManager* GenSourceManager = InPCGSubsystem->GetGenSourceManager();

	// Find-only: GetPCGWorldActor() lazily SPAWNS a transactional world actor into
	// editor worlds -- a PCG-free map must not grow one just because this plugin ticks.
	// When PCG is actually in use, the engine has already created/registered the actor.
	const APCGWorldActor* PCGWorldActor = InPCGSubsystem->FindPCGWorldActor();

	if (!GenSourceManager || !PCGWorldActor)
	{
		return;
	}

	bOutHasRuntimeGen = true;

	// Benign wrt the scheduler: sources are ticked at most once per dirty cycle,
	// whether we or the runtime-gen scheduler consume the dirty flag first.
	const TSet<IPCGGenSourceBase*> GenSources = GenSourceManager->GetAllGenSources(PCGWorldActor);
	ScratchGenSources.Reserve(GenSources.Num());

	for (IPCGGenSourceBase* GenSource : GenSources)
	{
		if (!GenSource)
		{
			continue;
		}

#if WITH_EDITOR
		// The editor camera's raw FEditorViewportClient* is only re-resolved when the scheduler
		// dirties the manager; a viewport closed while the scheduler sat idle leaves it dangling.
		if (const UPCGGenSourceEditorCamera* EditorCamera = Cast<UPCGGenSourceEditorCamera>(GenSource))
		{
			if (!PCGExSchedulingSubsystem::IsViewportClientLive(EditorCamera->EditorViewportClient))
			{
				continue;
			}
		}
#endif

		ScratchGenSources.Add(GenSource);
	}
}

void UPCGExSchedulingSubsystem::RebuildActiveSourcesSnapshot(const bool bHasRuntimeGen)
{
	if (!bHasRuntimeGen)
	{
		{
			FReadScopeLock ReadLock(ActiveSourcesLock);
			if (!ActiveSourcesSnapshot)
			{
				return;
			}
		}

		FWriteScopeLock WriteLock(ActiveSourcesLock);
		ActiveSourcesSnapshot.Reset();
		return;
	}

	// Channel table is revision-stable -- share one immutable copy across frames.
	const UPCGExSchedulingSettings* Settings = GetDefault<UPCGExSchedulingSettings>();
	if (!CachedChannelTable || CachedChannelTableRevision != Settings->GetRevision())
	{
		TSharedRef<FPCGExActiveSourcesSnapshot::FChannelTable> Table = MakeShared<FPCGExActiveSourcesSnapshot::FChannelTable>();
		Settings->GetChannelTable(*Table);
		CachedChannelTable = Table;
		CachedChannelTableRevision = Settings->GetRevision();
	}

	ScratchSources.Reset();

	for (IPCGGenSourceBase* GenSource : ScratchGenSources)
	{
		const TOptional<FVector> Position = GenSource->GetPosition();
		if (!Position.IsSet())
		{
			continue;
		}

		FPCGExActiveSourcesSnapshot::FSourceState& State = ScratchSources.Emplace_GetRef();
		State.Source = GenSource;
		State.Position = Position.GetValue();
		State.Mask = ResolveSourceMask(GenSource);
		State.bIsEditorCamera = IsEditorCameraSource(GenSource);
	}

	// Publish only on change -- spares worker readers the per-frame write barrier and refcount churn.
	{
		FReadScopeLock ReadLock(ActiveSourcesLock);
		if (ActiveSourcesSnapshot
			&& ActiveSourcesSnapshot->ChannelTable == CachedChannelTable
			&& ActiveSourcesSnapshot->Sources == ScratchSources)
		{
			return;
		}
	}

	TSharedRef<FPCGExActiveSourcesSnapshot> Building = MakeShared<FPCGExActiveSourcesSnapshot>();
	Building->ChannelTable = CachedChannelTable;
	Building->Sources = ScratchSources;

	FWriteScopeLock WriteLock(ActiveSourcesLock);
	ActiveSourcesSnapshot = Building;
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

#pragma region Source masks

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

	if (DeadKeys.IsEmpty() && Updates.IsEmpty())
	{
		return;
	}

	FWriteScopeLock WriteLock(SourceMasksLock);
	for (const FObjectKey& DeadKey : DeadKeys) { SourceMasks.Remove(DeadKey); }
	for (const TPair<FObjectKey, FResolvedSource>& Update : Updates) { SourceMasks.Add(Update.Key, Update.Value); }
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

	FWriteScopeLock WriteLock(SourceMasksLock);
	SourceMasks.Add(Key, FResolvedSource{Mask, Revision});

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
	check(IsInGameThread());

	if (!InSourceObject)
	{
		return;
	}

	FWriteScopeLock WriteLock(SourceMasksLock);
	SourceMasks.Remove(FObjectKey(InSourceObject));
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

#pragma endregion

#pragma region Target cache

TSharedPtr<FPCGExTargetCacheSlot> UPCGExSchedulingSubsystem::AcquireTargetSlot(const FPCGExTargetQuery& InQuery, const UObject* InReferencer)
{
	check(IsInGameThread());

	if (!InReferencer)
	{
		return nullptr;
	}

	FTargetCacheEntry* Entry = TargetCaches.Find(InQuery);

	if (!Entry)
	{
		TArray<TWeakObjectPtr<const AActor>> TagActors;
		if (!InQuery.TargetTag.IsNone())
		{
			TagActors = ScanTagActors(InQuery.TargetTag);
		}

		TMap<FObjectKey, FTransform> TrackedTransforms;
		TArray<FSoftObjectPath> UnresolvedTargets;
		TSharedPtr<const FPCGExTargetSnapshot> Snapshot = BuildTargetSnapshot(InQuery, TagActors, TrackedTransforms, UnresolvedTargets);

		const double Now = FPlatformTime::Seconds();

		Entry = &TargetCaches.Add(InQuery);
		Entry->Slot = MakeShared<FPCGExTargetCacheSlot>();
		Entry->Slot->Snapshot = MoveTemp(Snapshot);
		Entry->TrackedTransforms = MoveTemp(TrackedTransforms);
		Entry->UnresolvedTargets = MoveTemp(UnresolvedTargets);
		Entry->TagActors = MoveTemp(TagActors);
		Entry->LastTagScanTime = Now;
		Entry->LastTrackingCheckTime = Now;

		// Let the next tick fold the new entry into the maintenance schedule.
		NextTargetMaintenanceTime = 0.0;
	}

	Entry->Referencers.Add(FObjectKey(InReferencer), PCGExSchedulingSubsystem::FindConsumerKey(InReferencer));

	return Entry->Slot;
}

void UPCGExSchedulingSubsystem::ReleaseTargetSlot(const FPCGExTargetQuery& InQuery, const UObject* InReferencer)
{
	check(IsInGameThread());

	FTargetCacheEntry* Entry = InReferencer ? TargetCaches.Find(InQuery) : nullptr;
	if (!Entry)
	{
		return;
	}

	Entry->Referencers.Remove(FObjectKey(InReferencer));

	if (Entry->Referencers.IsEmpty())
	{
		RemoveTargetEntry(InQuery);
	}
}

void UPCGExSchedulingSubsystem::RemoveTargetEntry(const FPCGExTargetQuery& InQuery)
{
	check(IsInGameThread());

	if (const FTargetCacheEntry* Entry = TargetCaches.Find(InQuery))
	{
		if (Entry->Slot)
		{
			Entry->Slot->bOrphaned = true;
		}
		TargetCaches.Remove(InQuery);
	}
}

void UPCGExSchedulingSubsystem::TickTargetCacheMaintenance(const double InNow)
{
	check(IsInGameThread());

	TArray<FPCGExTargetQuery> DeadKeys;

	// Consumer refreshes are deferred past the loop: the engine refresh path never re-enters this
	// cache synchronously, but keeping map iteration free of outside calls costs nothing.
	TArray<TSet<FObjectKey>> PendingRefreshes;

	// Idle cadence with no entries; every entry pulls the next pass earlier.
	constexpr double IdleInterval = 1.0;
	double NextDueTime = InNow + IdleInterval;

	for (TPair<FPCGExTargetQuery, FTargetCacheEntry>& Pair : TargetCaches)
	{
		const FPCGExTargetQuery& Query = Pair.Key;
		FTargetCacheEntry& Entry = Pair.Value;

		for (TMap<FObjectKey, FObjectKey>::TIterator It(Entry.Referencers); It; ++It)
		{
			if (!IsValid(It.Key().ResolveObjectPtr()))
			{
				It.RemoveCurrent();
			}
		}

		if (Entry.Referencers.IsEmpty() || !Entry.Slot)
		{
			DeadKeys.Add(Query);
			continue;
		}

		const bool bHasTag = !Query.TargetTag.IsNone();
		const double TagDueTime = Entry.LastTagScanTime + Query.TagQueryInterval;
		const double TrackingDueTime = Entry.LastTrackingCheckTime + Query.TrackingInterval;
		const bool bTagScanDue = bHasTag && InNow >= TagDueTime;
		const bool bTrackingDue = InNow >= TrackingDueTime;

		bool bRebuild = false;

		// Amortized tag rescan.
		if (bTagScanDue)
		{
			TArray<TWeakObjectPtr<const AActor>> Found = ScanTagActors(Query.TargetTag);

			// Order-insensitive set comparison.
			TSet<const AActor*> Previous;
			for (const TWeakObjectPtr<const AActor>& Weak : Entry.TagActors)
			{
				if (const AActor* Actor = Weak.Get()) { Previous.Add(Actor); }
			}

			TSet<const AActor*> Current;
			for (const TWeakObjectPtr<const AActor>& Weak : Found)
			{
				if (const AActor* Actor = Weak.Get()) { Current.Add(Actor); }
			}

			bRebuild = Previous.Num() != Current.Num() || !Previous.Includes(Current);

			Entry.TagActors = MoveTemp(Found);
			Entry.LastTagScanTime = InNow;
		}

		if (bTrackingDue)
		{
			// Previously-unloaded explicit references that streamed in since the last build, then movement / lifetime.
			if (!bRebuild && HaveTargetsResolved(Entry.UnresolvedTargets))
			{
				bRebuild = true;
			}

			if (!bRebuild && Query.bTrackMovement && HaveTargetsMoved(Entry.TrackedTransforms, Query.MovementTolerance))
			{
				bRebuild = true;
			}

			Entry.LastTrackingCheckTime = InNow;
		}

		const double EntryTrackingDueTime = Entry.LastTrackingCheckTime + Query.TrackingInterval;
		NextDueTime = FMath::Min(NextDueTime, bHasTag ? FMath::Min(Entry.LastTagScanTime + Query.TagQueryInterval, EntryTrackingDueTime) : EntryTrackingDueTime);

		if (!bRebuild)
		{
			continue;
		}

		Entry.Slot->Snapshot = BuildTargetSnapshot(Query, Entry.TagActors, Entry.TrackedTransforms, Entry.UnresolvedTargets);

		// Runtime-gen change detection only reacts to gen source movement -- force a
		// cleanup+rescan so region changes are picked up while sources sit still.
		if (Query.bRefreshConsumers)
		{
			TSet<FObjectKey> Consumers;
			for (const TPair<FObjectKey, FObjectKey>& Referencer : Entry.Referencers)
			{
				if (Referencer.Value != FObjectKey())
				{
					Consumers.Add(Referencer.Value);
				}
			}

			if (!Consumers.IsEmpty())
			{
				PendingRefreshes.Add(MoveTemp(Consumers));
			}
		}
	}

	for (const FPCGExTargetQuery& DeadKey : DeadKeys)
	{
		RemoveTargetEntry(DeadKey);
	}

	for (const TSet<FObjectKey>& Consumers : PendingRefreshes)
	{
		RefreshTargetConsumers(Consumers);
	}

	NextTargetMaintenanceTime = NextDueTime;
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

		// PCG-aware bounds: excludes PCG-generated components, so a target hosting PCG
		// output doesn't inflate its own region (self-reinforcing feedback loop).
		const FBox Bounds = PCGHelpers::GetActorBounds(Actor);
		if (!Bounds.IsValid)
		{
			// No primitive components: nothing to region (an invalid box would land at the origin).
			continue;
		}

		FPCGExTargetShape& Shape = Snapshot->Shapes.Emplace_GetRef();
		Shape.Bounds = Bounds;

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

#pragma endregion

void UPCGExSchedulingSubsystem::DebugDraw(const UPCGSubsystem* InPCGSubsystem, const int32 InMode) const
{
#if UE_ENABLE_DEBUG_DRAWING
	check(IsInGameThread());

	const UWorld* World = GetWorld();

	for (const IPCGGraphExecutionSource* ExecutionSource : InPCGSubsystem->GetAllRegisteredExecutionSources())
	{
		if (!ExecutionSource)
		{
			continue;
		}

		// Same gate as the scheduler; local (partitioned cell) sources share their original's policy -- draw once per original.
		const IPCGGraphExecutionState& State = ExecutionSource->GetExecutionState();
		if (!State.IsManagedByRuntimeGenSystem() || State.IsLocalSource() || !State.GetGraph() || !State.IsActive())
		{
			continue;
		}

		if (const UPCGExSchedulingPolicy* Policy = Cast<UPCGExSchedulingPolicy>(State.GetRuntimeGenSchedulingPolicy()))
		{
			Policy->DebugDraw(World, ScratchGenSources, State.Use2DGrid(), InMode >= 2);
		}
	}
#endif
}
