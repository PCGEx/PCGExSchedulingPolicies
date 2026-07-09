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
	/** Region scale (hysteresis, inversion-aware). */
	double Scale = 1.0;

	/** True when evaluating the cleanup variant. */
	bool bExpanded = false;

	bool bUse2DGrid = false;

	/** True on the game thread — precise actor-touching tests are only allowed there. */
	bool bGameThread = false;
};

/**
 * Base for world-region target constraints: cells generate while they intersect regions
 * derived from target actors (bounds, volumes, splines). Targets are discovered through
 * explicit references and/or an amortized actor-tag query, resolved into immutable
 * snapshots by the scheduling subsystem, and optionally tracked for movement — moving
 * targets force a runtime-gen rescan of the owning component (engine change detection
 * only reacts to generation source movement).
 *
 * The generation source itself is ignored by these gates: source proximity remains the
 * engine's generation-radii broadphase (compose with shape constraints for more).
 */
UCLASS(Abstract)
class PCGEXSCHEDULINGPOLICIES_API UPCGExSchedulingConstraintTargetsBase : public UPCGExSchedulingConstraint
{
	GENERATED_BODY()

public:
	/** Explicit target actors (resolved when loaded — never force-loads). */
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
	//~ End UPCGExSchedulingConstraint interface

#if WITH_EDITOR
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
#endif

protected:
	/** Geometry this constraint extracts from its targets. */
	virtual EPCGExTargetGeometry GetTargetGeometry() const { return EPCGExTargetGeometry::Bounds; }

	/** Polyline flattening tolerance for Spline geometry. */
	virtual float GetSplineErrorTolerance() const { return 50.0f; }

	/** Cell-vs-region test. Must be worker-safe unless InContext.bGameThread. */
	virtual bool TestShape(const FPCGExTargetShape& InShape, const FBox& InBounds, const FPCGExTargetTestContext& InContext) const { return false; }

	/** Distance from the cell to the region, for priority. Game thread only. */
	virtual double DistanceToShape(const FPCGExTargetShape& InShape, const FBox& InBounds, bool bUse2DGrid) const { return TNumericLimits<double>::Max(); }

	/** Query descriptor handed to the subsystem registry. */
	FPCGExTargetQuery BuildTargetQuery() const;

	/** Current snapshot: resolves on the game thread, cached-only on workers. */
	TSharedPtr<const FPCGExTargetSnapshot> GetTargetSnapshot() const;

	/** Region scale for a gate evaluation: enlarged for regular cleanup, shrunk for inverted cleanup. */
	double GetRegionScale(bool bExpanded) const;
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
	virtual bool TestShape(const FPCGExTargetShape& InShape, const FBox& InBounds, const FPCGExTargetTestContext& InContext) const override;
	virtual double DistanceToShape(const FPCGExTargetShape& InShape, const FBox& InBounds, bool bUse2DGrid) const override;
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
	 * Applies to the game-thread generation path only — the worker-thread cleanup path
	 * always uses the bounds box, which makes cleanup slightly conservative.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = Settings)
	bool bPreciseVolumeTest = false;

protected:
	virtual EPCGExTargetGeometry GetTargetGeometry() const override { return EPCGExTargetGeometry::Volume; }
	virtual bool TestShape(const FPCGExTargetShape& InShape, const FBox& InBounds, const FPCGExTargetTestContext& InContext) const override;
	virtual double DistanceToShape(const FPCGExTargetShape& InShape, const FBox& InBounds, bool bUse2DGrid) const override;
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

	/** Max deviation when flattening splines to polylines. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = Settings, AdvancedDisplay, meta = (ClampMin = 1.0, Units = "cm"))
	float SplineErrorTolerance = 50.0f;

protected:
	virtual EPCGExTargetGeometry GetTargetGeometry() const override { return EPCGExTargetGeometry::Spline; }
	virtual float GetSplineErrorTolerance() const override { return SplineErrorTolerance; }
	virtual bool TestShape(const FPCGExTargetShape& InShape, const FBox& InBounds, const FPCGExTargetTestContext& InContext) const override;
	virtual double DistanceToShape(const FPCGExTargetShape& InShape, const FBox& InBounds, bool bUse2DGrid) const override;
};
