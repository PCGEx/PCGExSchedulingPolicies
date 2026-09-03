// Copyright 2026 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#pragma once

#include "CoreMinimal.h"

#include "PCGExSchedulingConstraint.h"

#include "PCGExSchedulingConstraintSource.generated.h"

/**
 * Priority-only constraint: cells aligned with the generation source's facing direction
 * are scheduled sooner (the stock policy's direction term). Never gates -- combine with
 * shapes for spatial masking. Invert to favor cells behind the source.
 */
UCLASS(BlueprintType, DisplayName = "Source : Direction Alignment")
class PCGEXSCHEDULINGPOLICIES_API UPCGExSchedulingConstraintDirectionAlignment : public UPCGExSchedulingConstraint
{
	GENERATED_BODY()

public:
	virtual TOptional<double> CalcPriority(const IPCGGenSourceBase* InGenSource, const FBox& InBounds, bool bUse2DGrid) const override;

	// Mirrors the stock policy: direction-based *priority* alone does not force
	// rescans on rotation -- only direction-based culling does (see View Frustum).
	virtual bool CullsBasedOnDirection() const override { return false; }
	virtual void DebugDraw(const UWorld* InWorld, const IPCGGenSourceBase* InGenSource, bool bUse2DGrid, bool bDrawCleanup) const override;
};

/**
 * Gate constraint replicating the stock policy's frustum culling: cells generate while
 * their bounds intersect the generation source's view frustum. Sources without a view
 * frustum are unaffected (always pass). Invert to generate only off-screen cells.
 */
UCLASS(BlueprintType, DisplayName = "Source : View Frustum")
class PCGEXSCHEDULINGPOLICIES_API UPCGExSchedulingConstraintViewFrustum : public UPCGExSchedulingConstraint
{
	GENERATED_BODY()

public:
	/** Multiplier to scale cell bounds by when comparing against the view frustum for generation. Can help if cells on the edge of the frustum generate later than you would like. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = Settings, meta = (ClampMin = 1.0))
	float GenerateBoundsModifier = 1.0f;

	/** Multiplier to scale cell bounds by when comparing against the view frustum for cleanup (hysteresis). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = Settings, meta = (ClampMin = 1.0))
	float CleanupBoundsModifier = 1.2f;

	virtual bool EvaluateGate(const IPCGGenSourceBase* InGenSource, const FBox& InBounds, bool bUse2DGrid, bool bExpanded) const override;
	virtual bool CullsBasedOnDirection() const override { return true; }
	virtual void DebugDraw(const UWorld* InWorld, const IPCGGenSourceBase* InGenSource, bool bUse2DGrid, bool bDrawCleanup) const override;
};
