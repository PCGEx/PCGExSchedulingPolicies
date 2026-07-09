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

	/** Squared distance from the FARTHEST point of a box to a point (Z ignored on 2D grids) -- 'cell fully inside sphere' tests. */
	inline double MaxSquaredDistanceToPoint(const FBox& InBounds, const FVector& InPoint, const bool bUse2DGrid)
	{
		const double DX = FMath::Max(FMath::Abs(InBounds.Min.X - InPoint.X), FMath::Abs(InBounds.Max.X - InPoint.X));
		const double DY = FMath::Max(FMath::Abs(InBounds.Min.Y - InPoint.Y), FMath::Abs(InBounds.Max.Y - InPoint.Y));
		const double DZ = bUse2DGrid ? 0.0 : FMath::Max(FMath::Abs(InBounds.Min.Z - InPoint.Z), FMath::Abs(InBounds.Max.Z - InPoint.Z));
		return DX * DX + DY * DY + DZ * DZ;
	}

	/** Null-safe source position fetch. */
	PCGEXSCHEDULINGPOLICIES_API TOptional<FVector> GetSourcePosition(const IPCGGenSourceBase* InGenSource);
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

	/** Only the shell of the shape gates generation -- cells fully inside the hollow interior are excluded. Affects gating only, not priority. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = Settings)
	bool bHollow = false;

	/** Shell thickness, measured inward from the shape surface. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = Settings, meta = (EditCondition = "bHollow", ClampMin = 1.0, Units = "cm"))
	double ShellThickness = 6400.0;

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

	/**
	 * Mirrored factor for the hollow interior boundary: the cleanup shell must widen on BOTH
	 * bounds (outer×Scale grows out, inner×(2-Scale) grows in) so neither shell edge flickers.
	 * Inverted constraints (Scale < 1) mirror it -- the outer bound shrinks while the inner
	 * bound grows, narrowing the shell from both sides.
	 */
	double GetInnerGateScale(const bool bExpanded) const
	{
		return FMath::Max(2.0 - GetGateScale(bExpanded), 0.0);
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
	/** Sphere radius around the generation source. Note: the component's generation radii remain the engine-level broadphase -- cells beyond them never generate. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = Settings, meta = (ClampMin = 1.0, Units = "cm"))
	double Radius = 25600.0;

	virtual bool EvaluateGate(const IPCGGenSourceBase* InGenSource, const FBox& InBounds, bool bUse2DGrid, bool bExpanded) const override;
	virtual TOptional<double> CalcPriority(const IPCGGenSourceBase* InGenSource, const FBox& InBounds, bool bUse2DGrid) const override;
};

/** Cells generate while they intersect a world-Z-aligned cylinder around the generation source -- vertical reach is decoupled from horizontal reach. */
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
 * The test is conservative-inclusive (cell bounding sphere vs cone) -- cells straddling the cone
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

protected:
	/**
	 * Value-keyed trig cache: recomputed when the angle changes, cached from the game thread
	 * only. Worker-thread cleanup either reads the cache or recomputes locally -- the
	 * scheduler's scan-then-cleanup barrier orders the game-thread write before worker reads.
	 */
	void GetHalfAngleTrig(double& OutCosHalfAngle, double& OutSinHalfAngle) const
	{
		const double ClampedAngle = FMath::Clamp(HalfAngleDegrees, 1.0, 89.0);

		if (CachedTrigAngle == ClampedAngle)
		{
			OutCosHalfAngle = CachedCosHalfAngle;
			OutSinHalfAngle = CachedSinHalfAngle;
			return;
		}

		const double HalfAngleRadians = FMath::DegreesToRadians(ClampedAngle);
		OutCosHalfAngle = FMath::Cos(HalfAngleRadians);
		OutSinHalfAngle = FMath::Sin(HalfAngleRadians);

		if (IsInGameThread())
		{
			CachedCosHalfAngle = OutCosHalfAngle;
			CachedSinHalfAngle = OutSinHalfAngle;
			CachedTrigAngle = ClampedAngle;
		}
	}

private:
	mutable double CachedTrigAngle = TNumericLimits<double>::Max();
	mutable double CachedCosHalfAngle = 0.0;
	mutable double CachedSinHalfAngle = 1.0;
};
