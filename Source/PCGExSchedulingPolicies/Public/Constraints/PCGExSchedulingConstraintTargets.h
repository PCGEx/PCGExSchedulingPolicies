// Copyright 2026 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#pragma once

#include "CoreMinimal.h"

#include "PCGExSchedulingConstraint.h"
#include "PCGExSchedulingSubsystem.h"

#include "PCGExSchedulingConstraintTargets.generated.h"

class AActor;

/** Evaluation context passed to target shape tests. */
struct FPCGExTargetTestContext
{
	/** True when evaluating the cleanup variant. */
	bool bExpanded = false;

	bool bUse2DGrid = false;

	/** True on the game thread -- precise actor-touching tests are only allowed there. */
	bool bGameThread = false;
};

/** Consumer-side padded regions of one target shape. */
struct FPCGExTargetRegion
{
	/** Shape bounds expanded by the constraint's bounds expansion -- the generate-gate broadphase. */
	FBox Generate = FBox(EForceInit::ForceInit);

	/** Generate scaled about its center by the cleanup scale -- the cleanup-gate broadphase. */
	FBox Cleanup = FBox(EForceInit::ForceInit);

	const FBox& Get(const bool bExpanded) const { return bExpanded ? Cleanup : Generate; }
};

/**
 * Base for world-region target constraints: cells generate while they intersect regions
 * derived from target actors (bounds, volumes, splines). Targets are discovered through
 * explicit references and/or an amortized actor-tag query, resolved into immutable
 * snapshots by the scheduling subsystem (shared between constraints with equal queries),
 * and optionally tracked for movement -- moving targets rebuild the snapshot and, when
 * bRefreshOnTargetChange is set, force a runtime-gen rescan of the owning component.
 *
 * Padding (bounds expansion, cleanup hysteresis) is baked per constraint from the shared
 * snapshot on the scan path, so runtime edits of those properties take effect on the next scan.
 *
 * The generation source itself is ignored by these gates: source proximity remains the
 * engine's generation-radii broadphase (compose with shape constraints for more).
 */
UCLASS(Abstract)
class PCGEXSCHEDULINGPOLICIES_API UPCGExSchedulingConstraintTargetsBase : public UPCGExSchedulingConstraint
{
	GENERATED_BODY()

public:
	/** Explicit target actors (resolved when loaded -- never force-loads). */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = Settings)
	TArray<TSoftObjectPtr<AActor>> TargetActors;

	/** Additionally discover targets by actor tag (amortized world scan). */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = Settings)
	bool bQueryByTag = false;

	/** Actor tag granting target status. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = Settings, meta = (EditCondition = "bQueryByTag"))
	FName TargetTag = NAME_None;

	/** Seconds between tag scans. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = Settings, meta = (EditCondition = "bQueryByTag", ClampMin = 0.1))
	float TagQueryInterval = 2.0f;

	/** Rebuild regions and rescan when a target moves or dies. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = Settings)
	bool bTrackTargetMovement = true;

	/** Movement detection tolerance. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = Settings, meta = (EditCondition = "bTrackTargetMovement", ClampMin = 1.0, Units = "cm"))
	float MovementTolerance = 50.0f;

	/** Seconds between movement and streaming checks on the resolved targets. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = Settings, meta = (ClampMin = 0.1))
	float TrackingInterval = 2.0f;

	/**
	 * Full cleanup + regenerate of the owning component whenever the target set changes (move, death, stream-in,
	 * tag scan). Disable for frequently moving targets and let generation source movement pick changes up.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = Settings)
	bool bRefreshOnTargetChange = true;

	/** Region scale applied when evaluating cleanup, creating hysteresis so cells don't flicker at region borders. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = Settings, meta = (ClampMin = 1.0))
	float CleanupExpansionScale = 1.1f;

	/** Distance over which priority falls off to 0 away from the regions. 0 disables this constraint's priority contribution. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = Settings, meta = (ClampMin = 0.0, Units = "cm"))
	float PriorityFalloffDistance = 25600.0f;

	/** Exponent applied to the normalized closeness for priority. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = Settings, meta = (ClampMin = 0.01))
	float FalloffExponent = 1.0f;

	//~ Begin UPCGExSchedulingConstraint interface
	virtual bool EvaluateGate(const IPCGGenSourceBase* InGenSource, const FBox& InBounds, bool bUse2DGrid, bool bExpanded) const override;
	virtual TOptional<double> CalcPriority(const IPCGGenSourceBase* InGenSource, const FBox& InBounds, bool bUse2DGrid) const override;
	virtual bool DebugDrawsPerSource() const override { return false; }
	virtual void DebugDraw(const UWorld* InWorld, const IPCGGenSourceBase* InGenSource, bool bUse2DGrid, bool bDrawCleanup) const override;
	//~ End UPCGExSchedulingConstraint interface

#if WITH_EDITOR
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
#endif

protected:
	/** Geometry this constraint extracts from its targets. */
	virtual EPCGExTargetGeometry GetTargetGeometry() const { return EPCGExTargetGeometry::Bounds; }

	/** Polyline flattening tolerance for Spline geometry. */
	virtual float GetSplineErrorTolerance() const { return 50.0f; }

	/** Cell-vs-region test. InRegion is this constraint's padded region for InShape. Must be worker-safe unless InContext.bGameThread. */
	virtual bool TestShape(const FPCGExTargetShape& InShape, const FBox& InRegion, const FBox& InBounds, const FPCGExTargetTestContext& InContext) const { return false; }

	/** Flat world-space expansion baked into every region (bounds expansion, spline corridor radius). */
	virtual float GetBoundsExpansion() const { return 0.0f; }

	/** Distance from the cell to the region, for priority -- shared box-based default. Game thread only. */
	virtual double DistanceToShape(const FPCGExTargetShape& InShape, const FBox& InGenerateRegion, const FBox& InBounds, bool bUse2DGrid) const;

	/** Query descriptor handed to the subsystem cache -- also its key, so equal queries share one snapshot. */
	FPCGExTargetQuery BuildTargetQuery() const;

	/**
	 * Scan-path accessor: acquires the subsystem slot on first use, then refreshes the cached
	 * snapshot and re-bakes the padded regions whenever the shared snapshot or the padding changed.
	 * Game thread, never from the cleanup path -- cleanup reads CachedSnapshot / CachedRegions as-is.
	 */
	const FPCGExTargetSnapshot* RefreshTargetSnapshot() const;

	/** Hands the slot back to the subsystem (constraint edited) so the next scan re-acquires with the new query. Game thread. */
	void ReleaseTargetSlot() const;

	/** Region scale for a gate evaluation: enlarged for regular cleanup, shrunk for inverted cleanup. */
	double GetRegionScale(bool bExpanded) const;

	/** Last scan's snapshot and regions, parallel arrays. Read-only on the cleanup path. */
	const FPCGExTargetSnapshot* GetCachedSnapshot() const { return CachedSnapshot.Get(); }
	const TArray<FPCGExTargetRegion>& GetCachedRegions() const { return CachedRegions; }

private:
	void BakeRegions() const;

	/** Subsystem cache slot for AcquiredQuery. Written on the scan path only. */
	mutable TSharedPtr<FPCGExTargetCacheSlot> CachedSlot;
	mutable TSharedPtr<const FPCGExTargetSnapshot> CachedSnapshot;
	mutable TArray<FPCGExTargetRegion> CachedRegions;
	mutable double CachedRegionExpansion = 0.0;
	mutable double CachedCleanupScale = 1.0;
	mutable FPCGExTargetQuery AcquiredQuery;
};

/** Cells generate while they overlap target actors' bounds. */
UCLASS(BlueprintType, DisplayName = "Target : Actor Bounds")
class PCGEXSCHEDULINGPOLICIES_API UPCGExSchedulingConstraintTargetBounds : public UPCGExSchedulingConstraintTargetsBase
{
	GENERATED_BODY()

public:
	/** Flat world-space expansion applied to each target's bounds. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = Settings, meta = (Units = "cm"))
	float BoundsExpansion = 0.0f;

protected:
	virtual float GetBoundsExpansion() const override { return BoundsExpansion; }
	virtual bool TestShape(const FPCGExTargetShape& InShape, const FBox& InRegion, const FBox& InBounds, const FPCGExTargetTestContext& InContext) const override;
};

/** Cells generate while they overlap target volumes. Bounds-box test by default; optional precise brush test on the generation path. */
UCLASS(BlueprintType, DisplayName = "Target : Volume")
class PCGEXSCHEDULINGPOLICIES_API UPCGExSchedulingConstraintTargetVolume : public UPCGExSchedulingConstraintTargetsBase
{
	GENERATED_BODY()

public:
	/** Flat world-space expansion applied to each volume's bounds. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = Settings, meta = (Units = "cm"))
	float BoundsExpansion = 0.0f;

	/**
	 * Precise brush containment test (cell center + radius against the volume brush).
	 * Applies to the game-thread generation path on 3D grids only -- 2D grids and the
	 * worker-thread cleanup path always use the bounds box, which makes cleanup slightly
	 * conservative. Ignored when Invert is set (the box/brush asymmetry would make
	 * inverted cells flicker between generate and cleanup).
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = Settings)
	bool bPreciseVolumeTest = false;

protected:
	virtual EPCGExTargetGeometry GetTargetGeometry() const override { return EPCGExTargetGeometry::Volume; }
	virtual float GetBoundsExpansion() const override { return BoundsExpansion; }
	virtual bool TestShape(const FPCGExTargetShape& InShape, const FBox& InRegion, const FBox& InBounds, const FPCGExTargetTestContext& InContext) const override;
};

/** Cells generate while they are within a corridor radius of target splines. Actors without splines fall back to their bounds. */
UCLASS(BlueprintType, DisplayName = "Target : Spline")
class PCGEXSCHEDULINGPOLICIES_API UPCGExSchedulingConstraintTargetSpline : public UPCGExSchedulingConstraintTargetsBase
{
	GENERATED_BODY()

public:
	/** Corridor radius around target splines. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = Settings, meta = (ClampMin = 1.0, Units = "cm"))
	float SplineRadius = 1000.0f;

	/**
	 * Also treat the interior of closed splines as inside (XY polygon fill; vertical reach spans the
	 * spline's Z range plus the corridor radius). The corridor provides the boundary hysteresis.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = Settings)
	bool bFillClosedSplines = false;

	/** Max deviation when flattening splines to polylines. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = Settings, AdvancedDisplay, meta = (ClampMin = 1.0, Units = "cm"))
	float SplineErrorTolerance = 50.0f;

protected:
	virtual EPCGExTargetGeometry GetTargetGeometry() const override { return EPCGExTargetGeometry::Spline; }
	virtual float GetSplineErrorTolerance() const override { return SplineErrorTolerance; }
	/** The corridor radius pads the polyline bounds into the broadphase region. */
	virtual float GetBoundsExpansion() const override { return SplineRadius; }
	virtual bool TestShape(const FPCGExTargetShape& InShape, const FBox& InRegion, const FBox& InBounds, const FPCGExTargetTestContext& InContext) const override;
	virtual double DistanceToShape(const FPCGExTargetShape& InShape, const FBox& InGenerateRegion, const FBox& InBounds, bool bUse2DGrid) const override;
};
