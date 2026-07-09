// Copyright 2026 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#pragma once

#include "CoreMinimal.h"

#include "PCGExSchedulingConstraint.h"

#include "PCGExSchedulingConstraintShapes.generated.h"

namespace PCGExSchedulingShapes
{
	/** Source position flattened onto the cell plane for 2D grids (scheduler convention: Z = bounds min). */
	inline FVector SanitizedSourcePosition(const FVector& InPosition, const FBox& InBounds, const bool bUse2DGrid)
	{
		FVector OutPosition = InPosition;
		if (bUse2DGrid) { OutPosition.Z = InBounds.Min.Z; }
		return OutPosition;
	}
}

/**
 * Base for source-centered shape constraints: cells generate while they intersect the shape
 * around the generation source, with cleanup hysteresis and priority-by-closeness falloff.
 */
UCLASS(Abstract)
class PCGEXSCHEDULINGPOLICIES_API UPCGExSchedulingConstraintShape : public UPCGExSchedulingConstraint
{
	GENERATED_BODY()

public:
	/** Scale applied to the shape when evaluating cleanup, creating hysteresis so cells don't flicker at the boundary. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = Settings, meta = (ClampMin = 1.0))
	double CleanupScale = 1.1;

	/** Exponent applied to the normalized closeness for priority (1 = linear, >1 = favors cells near the source more sharply). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = Settings, meta = (ClampMin = 0.01))
	double FalloffExponent = 1.0;

protected:
	/** Shape scale for a gate evaluation: enlarged for regular cleanup, shrunk for inverted cleanup. */
	double GetGateScale(const bool bExpanded) const
	{
		if (!bExpanded) { return 1.0; }
		const double Scale = FMath::Max(CleanupScale, 1.0);
		return bInvert ? 1.0 / Scale : Scale;
	}

	/** Falloff-shaped priority from a normalized closeness, honoring bInvert. */
	double ShapedPriority(const double InNormalizedCloseness) const
	{
		double Closeness = FMath::Clamp(InNormalizedCloseness, 0.0, 1.0);
		if (bInvert) { Closeness = 1.0 - Closeness; }
		return FMath::Pow(Closeness, FalloffExponent);
	}
};

/** Cells generate while they intersect a sphere around the generation source. */
UCLASS(BlueprintType, DisplayName = "Shape : Sphere")
class PCGEXSCHEDULINGPOLICIES_API UPCGExSchedulingConstraintSphere : public UPCGExSchedulingConstraintShape
{
	GENERATED_BODY()

public:
	/** Sphere radius around the generation source. Note: the component's generation radii remain the engine-level broadphase — cells beyond them never generate. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = Settings, meta = (ClampMin = 1.0, Units = "cm"))
	double Radius = 25600.0;

	virtual bool EvaluateGate(const IPCGGenSourceBase* InGenSource, const FBox& InBounds, bool bUse2DGrid, bool bExpanded) const override;
	virtual TOptional<double> CalcPriority(const IPCGGenSourceBase* InGenSource, const FBox& InBounds, bool bUse2DGrid) const override;
};

/** Cells generate while they intersect a world-Z-aligned cylinder around the generation source — vertical reach is decoupled from horizontal reach. */
UCLASS(BlueprintType, DisplayName = "Shape : Cylinder")
class PCGEXSCHEDULINGPOLICIES_API UPCGExSchedulingConstraintCylinder : public UPCGExSchedulingConstraintShape
{
	GENERATED_BODY()

public:
	/** Cylinder radius (XY plane) around the generation source. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = Settings, meta = (ClampMin = 1.0, Units = "cm"))
	double Radius = 25600.0;

	/** Cylinder half height, centered on the generation source. Ignored on 2D grids. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = Settings, meta = (ClampMin = 1.0, Units = "cm"))
	double HalfHeight = 12800.0;

	virtual bool EvaluateGate(const IPCGGenSourceBase* InGenSource, const FBox& InBounds, bool bUse2DGrid, bool bExpanded) const override;
	virtual TOptional<double> CalcPriority(const IPCGGenSourceBase* InGenSource, const FBox& InBounds, bool bUse2DGrid) const override;
};

/** Cells generate while they intersect a box around the generation source, optionally yaw-aligned to its facing direction. */
UCLASS(BlueprintType, DisplayName = "Shape : Box")
class PCGEXSCHEDULINGPOLICIES_API UPCGExSchedulingConstraintBox : public UPCGExSchedulingConstraintShape
{
	GENERATED_BODY()

public:
	/** Box half-extents around the generation source (X = forward when aligned to the source direction). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = Settings, meta = (ClampMin = 1.0, Units = "cm"))
	FVector Extents = FVector(25600.0, 25600.0, 12800.0);

	/** Yaw-align the box to the source's facing direction (horizontal only). Falls back to world-aligned when the source has no usable direction. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = Settings)
	bool bAlignToSourceDirection = false;

	virtual bool EvaluateGate(const IPCGGenSourceBase* InGenSource, const FBox& InBounds, bool bUse2DGrid, bool bExpanded) const override;
	virtual TOptional<double> CalcPriority(const IPCGGenSourceBase* InGenSource, const FBox& InBounds, bool bUse2DGrid) const override;
	virtual bool CullsBasedOnDirection() const override { return bAlignToSourceDirection; }
};

/**
 * Cells generate while they intersect a cone from the generation source along its facing direction.
 * The test is conservative-inclusive (cell bounding sphere vs cone) — cells straddling the cone
 * surface are treated as inside. Sources without a usable direction fall back to a sphere of Range.
 */
UCLASS(BlueprintType, DisplayName = "Shape : Cone")
class PCGEXSCHEDULINGPOLICIES_API UPCGExSchedulingConstraintCone : public UPCGExSchedulingConstraintShape
{
	GENERATED_BODY()

public:
	/** Cone length from the generation source along its facing direction. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = Settings, meta = (ClampMin = 1.0, Units = "cm"))
	double Range = 25600.0;

	/** Cone half angle, in degrees. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = Settings, meta = (ClampMin = 1.0, ClampMax = 89.0, Units = "deg"))
	double HalfAngleDegrees = 45.0;

	virtual bool EvaluateGate(const IPCGGenSourceBase* InGenSource, const FBox& InBounds, bool bUse2DGrid, bool bExpanded) const override;
	virtual TOptional<double> CalcPriority(const IPCGGenSourceBase* InGenSource, const FBox& InBounds, bool bUse2DGrid) const override;
	virtual bool CullsBasedOnDirection() const override { return true; }
};
