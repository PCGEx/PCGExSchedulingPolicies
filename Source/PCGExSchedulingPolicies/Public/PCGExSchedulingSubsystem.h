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

/** Which geometry a target query extracts from its actors. */
enum class EPCGExTargetGeometry : uint8
{
	Bounds = 0,
	Volume,
	Spline
};

/** Immutable description of a target set — what to resolve and how to track it. */
struct FPCGExTargetQuery
{
	/** Explicit actor references (resolved when loaded — never force-loads). */
	TArray<FSoftObjectPath> TargetActors;

	/** Optional world tag query. */
	FName TargetTag = NAME_None;

	/** Seconds between tag scans. */
	float TagQueryInterval = 2.0f;

	/** Geometry extracted from each resolved actor. */
	EPCGExTargetGeometry Geometry = EPCGExTargetGeometry::Bounds;

	/** Max deviation (cm) when flattening splines to polylines. */
	float SplineErrorTolerance = 50.0f;

	/** Rebuild + rescan consumers when a target moves. */
	bool bTrackMovement = true;

	/** Movement detection tolerance (cm). */
	float MovementTolerance = 50.0f;
};

/** One resolved target region — pure math except the weak volume pointer (game-thread deref only). */
struct FPCGExTargetShape
{
	/** World-space bounds (points bounds for splines). */
	FBox Bounds = FBox(EForceInit::ForceInit);

	/** Set for Volume geometry — game-thread precise tests only. */
	TWeakObjectPtr<const AVolume> Volume;

	/** World-space polyline, Spline geometry only. */
	TArray<FVector> SplinePoints;

	bool bClosedSpline = false;
};

/** Immutable snapshot of a resolved target set. Shared to worker threads by pointer. */
struct FPCGExTargetSnapshot
{
	TArray<FPCGExTargetShape> Shapes;
};

/**
 * Per-frame snapshot of the world's active generation sources (positions + channel masks)
 * and the channel table, for channel-aware graph nodes executing on worker threads.
 */
struct FPCGExActiveSourcesSnapshot
{
	struct FSourceState
	{
		FVector Position = FVector::ZeroVector;
		PCGExScheduling::FChannelMask Mask = 0;
		bool bIsEditorCamera = false;
	};

	TArray<FSourceState> Sources;

	/** Settings-ordered (name, single-bit mask) channel table, revision-consistent with the source masks. */
	TArray<TPair<FName, PCGExScheduling::FChannelMask>> ChannelTable;

	PCGExScheduling::FChannelMask ResolveNames(const TArray<FName>& InNames) const
	{
		PCGExScheduling::FChannelMask Mask = 0;
		for (const FName& Name : InNames)
		{
			for (const TPair<FName, PCGExScheduling::FChannelMask>& Entry : ChannelTable)
			{
				if (Entry.Key == Name)
				{
					Mask |= Entry.Value;
					break;
				}
			}
		}
		return Mask;
	}
};

/**
 * World subsystem backing PCGEx scheduling policies: resolves and caches
 * per-generation-source channel masks.
 *
 * Threading contract:
 * - ResolveSourceMask() is game-thread only (touches actors and settings).
 * - GetCachedSourceMask() is safe from any thread (lock-protected read of the last resolved value).
 *   The runtime-gen scheduler evaluates ShouldGenerate on the game thread and ShouldCull on worker
 *   threads; cleanup only ever visits cells a prior game-thread scan already resolved, so worker
 *   reads find a (possibly one-tick stale) entry.
 */
UCLASS()
class PCGEXSCHEDULINGPOLICIES_API UPCGExSchedulingSubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	/** Returns the subsystem for the given world, if it exists and is initialized. */
	static UPCGExSchedulingSubsystem* GetInstance(const UWorld* InWorld);

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

	/** Drops the cached entry for a source so the next resolution recomputes it (e.g. channels edited on a component). */
	void InvalidateSource(const UObject* InSourceObject);

	/**
	 * Returns the target snapshot for the given key (typically the querying constraint), building it
	 * on first use via the lazily-invoked query provider. Registers the consumer's owning execution
	 * source for movement-driven rescans. Game thread only.
	 */
	TSharedPtr<const FPCGExTargetSnapshot> ResolveTargetSnapshot(const UObject* InKey, TFunctionRef<FPCGExTargetQuery()> InQueryProvider, const UObject* InConsumer);

	/** Last built snapshot for the given key, if any. Safe from any thread; never resolves. */
	TSharedPtr<const FPCGExTargetSnapshot> GetCachedTargetSnapshot(const UObject* InKey) const;

	/** Drops the cached target snapshot for a key so the next resolution rebuilds it (e.g. constraint edited). */
	void InvalidateTargetCache(const UObject* InKey);

	/** Latest per-frame snapshot of active generation sources. Safe from any thread. Null when the world has no PCG runtime activity. */
	TSharedPtr<const FPCGExActiveSourcesSnapshot> GetActiveSourcesSnapshot() const;

protected:
	/** Rebuilt every tick (cheap — a handful of sources). Game thread. */
	void RebuildActiveSourcesSnapshot();

	/** Uncached resolution ladder. Game thread only. */
	PCGExScheduling::FChannelMask ResolveMaskInternal(const IPCGGenSourceBase* InGenSource) const;

	struct FResolvedSource
	{
		PCGExScheduling::FChannelMask Mask = 0;
		uint32 Revision = 0;
	};

	struct FTargetCacheEntry
	{
		FPCGExTargetQuery Query;
		TSharedPtr<const FPCGExTargetSnapshot> Snapshot;

		/** Resolved actor → transform at snapshot time, for movement detection. */
		TMap<FObjectKey, FTransform> TrackedTransforms;

		/** Actors found by the last tag scan. */
		TArray<TWeakObjectPtr<const AActor>> TagActors;

		/** Execution sources to rescan when the target set changes. */
		TSet<FObjectKey> Consumers;

		double LastTagScanTime = 0.0;
	};

	void TickSourceMaskMaintenance(double InNow);
	void TickTargetCacheMaintenance(double InNow);

	/** Scans the world for actors carrying the query tag. Game thread only. */
	TArray<TWeakObjectPtr<const AActor>> ScanTagActors(FName InTag) const;

	/** Builds a snapshot from the query + tag actors; fills the tracked transforms. Game thread only. */
	TSharedPtr<const FPCGExTargetSnapshot> BuildTargetSnapshot(const FPCGExTargetQuery& InQuery, const TArray<TWeakObjectPtr<const AActor>>& InTagActors, TMap<FObjectKey, FTransform>& OutTrackedTransforms) const;

	/** True when any tracked actor died or moved past the query tolerances. Game thread only. */
	bool HaveTargetsMoved(const FPCGExTargetQuery& InQuery, const TMap<FObjectKey, FTransform>& InTrackedTransforms) const;

	/** Forces a runtime-gen rescan of every consumer. Game thread only. */
	void RefreshTargetConsumers(const TSet<FObjectKey>& InConsumers) const;

	mutable FRWLock SourceMasksLock;
	TMap<FObjectKey, FResolvedSource> SourceMasks;

	mutable FRWLock TargetCachesLock;
	TMap<FObjectKey, FTargetCacheEntry> TargetCaches;

	mutable FRWLock ActiveSourcesLock;
	TSharedPtr<const FPCGExActiveSourcesSnapshot> ActiveSourcesSnapshot;

	/** Last periodic maintenance timestamp (re-resolution + dead key cleanup). */
	double LastMaintenanceTime = 0.0;
};
