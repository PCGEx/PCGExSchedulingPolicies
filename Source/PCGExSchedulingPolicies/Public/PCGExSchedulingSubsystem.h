// Copyright 2026 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#pragma once

#include "CoreMinimal.h"
#include "HAL/CriticalSection.h"
#include "Misc/ScopeRWLock.h"
#include "Subsystems/WorldSubsystem.h"
#include "UObject/ObjectKey.h"
#include "UObject/SoftObjectPath.h"

#include "PCGExSchedulingCommon.h"

#include "PCGExSchedulingSubsystem.generated.h"

class AActor;
class AVolume;
class IPCGGenSourceBase;
class UPCGSubsystem;

/** Which geometry a target query extracts from its actors. */
enum class EPCGExTargetGeometry : uint8
{
	Bounds = 0,
	Volume,
	Spline
};

/**
 * Immutable description of a target set -- what to resolve and how to track it. Doubles as the
 * cache key: constraints with equal queries share one snapshot (padding is applied consumer-side).
 */
struct FPCGExTargetQuery
{
	/** Explicit actor references (resolved when loaded -- never force-loads). */
	TArray<FSoftObjectPath> TargetActors;

	/** Optional world tag query. */
	FName TargetTag = NAME_None;

	/** Seconds between tag scans. */
	float TagQueryInterval = 2.0f;

	/** Geometry extracted from each resolved actor. */
	EPCGExTargetGeometry Geometry = EPCGExTargetGeometry::Bounds;

	/** Max deviation (cm) when flattening splines to polylines. */
	float SplineErrorTolerance = 50.0f;

	/** Rebuild when a target moves. */
	bool bTrackMovement = true;

	/** Movement detection tolerance (cm). */
	float MovementTolerance = 50.0f;

	/** Seconds between movement and streaming checks. */
	float TrackingInterval = 2.0f;

	/** Force a runtime-gen rescan (full cleanup + regenerate) of consumers whenever the target set changes. */
	bool bRefreshConsumers = true;

	bool operator==(const FPCGExTargetQuery& Other) const
	{
		return TargetTag == Other.TargetTag
			&& TagQueryInterval == Other.TagQueryInterval
			&& Geometry == Other.Geometry
			&& SplineErrorTolerance == Other.SplineErrorTolerance
			&& bTrackMovement == Other.bTrackMovement
			&& MovementTolerance == Other.MovementTolerance
			&& TrackingInterval == Other.TrackingInterval
			&& bRefreshConsumers == Other.bRefreshConsumers
			&& TargetActors == Other.TargetActors;
	}

	bool operator!=(const FPCGExTargetQuery& Other) const { return !(*this == Other); }

	friend uint32 GetTypeHash(const FPCGExTargetQuery& In)
	{
		uint32 Hash = GetTypeHash(In.TargetTag);
		Hash = HashCombineFast(Hash, GetTypeHash(In.TagQueryInterval));
		Hash = HashCombineFast(Hash, GetTypeHash(static_cast<uint8>(In.Geometry)));
		Hash = HashCombineFast(Hash, GetTypeHash(In.SplineErrorTolerance));
		Hash = HashCombineFast(Hash, GetTypeHash(static_cast<uint8>(In.bTrackMovement)));
		Hash = HashCombineFast(Hash, GetTypeHash(In.MovementTolerance));
		Hash = HashCombineFast(Hash, GetTypeHash(In.TrackingInterval));
		Hash = HashCombineFast(Hash, GetTypeHash(static_cast<uint8>(In.bRefreshConsumers)));
		Hash = HashCombineFast(Hash, GetTypeHash(In.TargetActors));
		return Hash;
	}
};

/** One resolved target region -- pure math except the weak volume pointer (game-thread deref only). */
struct FPCGExTargetShape
{
	/** World-space bounds (points bounds for splines). Always valid. */
	FBox Bounds = FBox(EForceInit::ForceInit);

	/** Set for Volume geometry -- game-thread precise tests only. */
	TWeakObjectPtr<const AVolume> Volume;

	/** World-space polyline, Spline geometry only. */
	TArray<FVector> SplinePoints;

	bool bClosedSpline = false;

	int32 NumSegments() const { return bClosedSpline ? SplinePoints.Num() : SplinePoints.Num() - 1; }
	int32 NextPointIndex(const int32 InIndex) const { return InIndex + 1 == SplinePoints.Num() ? 0 : InIndex + 1; }
};

/** Immutable snapshot of a resolved target set. Shared to worker threads by pointer. */
struct FPCGExTargetSnapshot
{
	TArray<FPCGExTargetShape> Shapes;
};

/**
 * Handle shared between the subsystem's target cache and its consumers. Game thread only: the
 * subsystem swaps Snapshot on rebuild, consumers compare it against their own copy on the scan
 * path. Orphaned once the owning cache entry is gone -- consumers must then re-acquire.
 */
struct FPCGExTargetCacheSlot
{
	TSharedPtr<const FPCGExTargetSnapshot> Snapshot;
	bool bOrphaned = false;
};

/**
 * Per-frame snapshot of the world's active generation sources (positions + channel masks)
 * and the channel table, for channel-aware graph nodes executing on worker threads.
 */
struct FPCGExActiveSourcesSnapshot
{
	struct FSourceState
	{
		/** Identity only -- never dereferenced off the game thread. */
		const IPCGGenSourceBase* Source = nullptr;
		FVector Position = FVector::ZeroVector;
		PCGExScheduling::FChannelMask Mask = 0;
		bool bIsEditorCamera = false;

		bool operator==(const FSourceState& Other) const
		{
			return Source == Other.Source && Position == Other.Position && Mask == Other.Mask && bIsEditorCamera == Other.bIsEditorCamera;
		}
	};

	using FChannelTable = TArray<TPair<FName, PCGExScheduling::FChannelMask>>;

	TArray<FSourceState> Sources;

	/** Settings-ordered (name, single-bit mask) channel table, shared across frames -- rebuilt only on settings revision changes. */
	TSharedPtr<const FChannelTable> ChannelTable;

	/** Mask of a source by identity; unset when the source was not active when the snapshot was taken. Linear over a handful of sources. */
	TOptional<PCGExScheduling::FChannelMask> FindMask(const IPCGGenSourceBase* InSource) const
	{
		for (const FSourceState& State : Sources)
		{
			if (State.Source == InSource)
			{
				return State.Mask;
			}
		}
		return TOptional<PCGExScheduling::FChannelMask>();
	}

	PCGExScheduling::FChannelMask ResolveName(const FName InName) const
	{
		if (ChannelTable)
		{
			for (const TPair<FName, PCGExScheduling::FChannelMask>& Entry : *ChannelTable)
			{
				if (Entry.Key == InName)
				{
					return Entry.Value;
				}
			}
		}
		return 0;
	}

	PCGExScheduling::FChannelMask ResolveNames(const TArray<FName>& InNames) const
	{
		PCGExScheduling::FChannelMask Mask = 0;
		for (const FName& Name : InNames)
		{
			Mask |= ResolveName(Name);
		}
		return Mask;
	}
};

/**
 * World subsystem backing PCGEx scheduling policies: resolves and caches per-generation-source
 * channel masks, owns the shared target-region cache, and publishes the active-sources snapshot.
 *
 * Threading contract:
 * - Source masks: ResolveSourceMask() is game-thread only; GetCachedSourceMask() is safe from any
 *   thread (lock-protected read of the last resolved value, possibly one tick stale).
 * - Target cache: game thread only, no locks. Constraints touch it on the scan path exclusively;
 *   the cleanup path (which the game thread and workers run concurrently) reads constraint-owned copies.
 * - Active-sources snapshot: any thread.
 */
UCLASS()
class PCGEXSCHEDULINGPOLICIES_API UPCGExSchedulingSubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	/** Returns the subsystem for the given world, if it exists and is initialized. */
	static UPCGExSchedulingSubsystem* GetInstance(const UWorld* InWorld);

	//~ Begin USubsystem interface
	virtual void Deinitialize() override;
	//~ End USubsystem interface

	//~ Begin UTickableWorldSubsystem interface
	virtual void Tick(float DeltaSeconds) override;
	virtual ETickableTickType GetTickableTickType() const override { return IsTemplate() ? ETickableTickType::Never : ETickableTickType::Conditional; }
	virtual bool IsTickable() const override;
	virtual bool IsTickableInEditor() const override { return true; }
	virtual TStatId GetStatId() const override;
	//~ End UTickableWorldSubsystem interface

	/** Resolves (and caches) the channel mask of a generation source. Game thread only. */
	PCGExScheduling::FChannelMask ResolveSourceMask(const IPCGGenSourceBase* InGenSource);

	/** Last resolved channel mask of a generation source, if any. Safe from any thread; never resolves. */
	TOptional<PCGExScheduling::FChannelMask> GetCachedSourceMask(const IPCGGenSourceBase* InGenSource) const;

	/** Resolves on the game thread, falls back to the cached value on worker threads. */
	TOptional<PCGExScheduling::FChannelMask> GetOrResolveSourceMask(const IPCGGenSourceBase* InGenSource);

	/** Drops the cached entry for a source so the next resolution recomputes it (e.g. channels edited on a component). Game thread only. */
	void InvalidateSource(const UObject* InSourceObject);

	/**
	 * Returns the cache slot for a query, building its snapshot on first use. Registers the
	 * referencer (the constraint) so the entry stays alive, and the referencer's owning execution
	 * source for change-driven rescans. Game thread only.
	 */
	TSharedPtr<FPCGExTargetCacheSlot> AcquireTargetSlot(const FPCGExTargetQuery& InQuery, const UObject* InReferencer);

	/** Drops a referencer from a query's entry; the entry and its slot die with the last referencer. Game thread only. */
	void ReleaseTargetSlot(const FPCGExTargetQuery& InQuery, const UObject* InReferencer);

	/** Latest per-frame snapshot of active generation sources. Safe from any thread. Null when the world has no PCG runtime activity. */
	TSharedPtr<const FPCGExActiveSourcesSnapshot> GetActiveSourcesSnapshot() const;

	/** Single home for editor-camera identity -- the policy bypass and the node snapshot must agree on it. */
	static bool IsEditorCameraSource(const IPCGGenSourceBase* InGenSource);

protected:
	/** Live generation sources for this frame (editor camera included only while its viewport client is alive). Game thread. */
	void GatherGenSources(UPCGSubsystem* InPCGSubsystem, bool& bOutHasRuntimeGen);

	/** Refreshed every tick into ScratchSources; the shared snapshot is only allocated and republished on change. Game thread. */
	void RebuildActiveSourcesSnapshot(bool bHasRuntimeGen);

	/** Uncached resolution ladder. Game thread only. */
	PCGExScheduling::FChannelMask ResolveMaskInternal(const IPCGGenSourceBase* InGenSource) const;

	struct FResolvedSource
	{
		PCGExScheduling::FChannelMask Mask = 0;
		uint32 Revision = 0;
	};

	struct FTargetCacheEntry
	{
		TSharedPtr<FPCGExTargetCacheSlot> Slot;

		/** Resolved actor → transform at snapshot time, for movement detection. */
		TMap<FObjectKey, FTransform> TrackedTransforms;

		/** Explicit references that did not resolve at build time (unloaded) -- re-attempted during maintenance so streamed-in targets are picked up. */
		TArray<FSoftObjectPath> UnresolvedTargets;

		/** Actors found by the last tag scan. */
		TArray<TWeakObjectPtr<const AActor>> TagActors;

		/** Constraint holding the slot → its owning execution source (empty key when none). The entry dies with its last live referencer. */
		TMap<FObjectKey, FObjectKey> Referencers;

		double LastTagScanTime = 0.0;
		double LastTrackingCheckTime = 0.0;
	};

	void TickSourceMaskMaintenance();
	void TickTargetCacheMaintenance(double InNow);

	/** Orphans the entry's slot and removes it. The only removal path. Game thread only. */
	void RemoveTargetEntry(const FPCGExTargetQuery& InQuery);

	/** Scans the world for actors carrying the query tag. Game thread only. */
	TArray<TWeakObjectPtr<const AActor>> ScanTagActors(FName InTag) const;

	/** Builds a snapshot from the query + tag actors; fills the tracked transforms and the unresolved reference list. Game thread only. */
	TSharedPtr<const FPCGExTargetSnapshot> BuildTargetSnapshot(const FPCGExTargetQuery& InQuery, const TArray<TWeakObjectPtr<const AActor>>& InTagActors, TMap<FObjectKey, FTransform>& OutTrackedTransforms, TArray<FSoftObjectPath>& OutUnresolvedTargets) const;

	/** True when any tracked actor died or moved past the tolerance. Game thread only. */
	bool HaveTargetsMoved(const TMap<FObjectKey, FTransform>& InTrackedTransforms, double InMovementTolerance) const;

	/** True when any previously-unresolved explicit reference now resolves (target streamed in). Game thread only. */
	bool HaveTargetsResolved(const TArray<FSoftObjectPath>& InUnresolvedTargets) const;

	/** Forces a runtime-gen rescan of every consumer. Game thread only. */
	void RefreshTargetConsumers(const TSet<FObjectKey>& InConsumers) const;

	/** Draws every PCGEx policy's constraints for the runtime-generated components of this world. Game thread. */
	void DebugDraw(const UPCGSubsystem* InPCGSubsystem, int32 InMode) const;

	mutable FRWLock SourceMasksLock;
	TMap<FObjectKey, FResolvedSource> SourceMasks;

	/** Game thread only. */
	TMap<FPCGExTargetQuery, FTargetCacheEntry> TargetCaches;

	/** Earliest time any target entry is due for a tag scan or a tracking check. 0 forces a pass. */
	double NextTargetMaintenanceTime = 0.0;

	mutable FRWLock ActiveSourcesLock;
	TSharedPtr<const FPCGExActiveSourcesSnapshot> ActiveSourcesSnapshot;

	/** Game-thread scratch for the per-frame source gather. */
	TArray<IPCGGenSourceBase*> ScratchGenSources;
	TArray<FPCGExActiveSourcesSnapshot::FSourceState> ScratchSources;

	/** Settings channel table shared into snapshots -- rebuilt only when the settings revision changes. Game thread. */
	TSharedPtr<const FPCGExActiveSourcesSnapshot::FChannelTable> CachedChannelTable;
	uint32 CachedChannelTableRevision = 0;

	/** Last source-mask maintenance timestamp (re-resolution + dead key cleanup). */
	double LastMaintenanceTime = 0.0;
};
