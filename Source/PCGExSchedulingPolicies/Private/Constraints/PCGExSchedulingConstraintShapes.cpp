// Copyright 2026 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#include "Constraints/PCGExSchedulingConstraintShapes.h"

#include "RuntimeGen/GenSources/PCGGenSourceBase.h"

#include "Math/Box2D.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(PCGExSchedulingConstraintShapes)

namespace PCGExSchedulingConstraintShapes
{
	/** Source facing flattened to a usable XY yaw direction; unset when degenerate (e.g. looking straight down). */
	TOptional<FVector2D> GetYawDirection(const IPCGGenSourceBase* InGenSource)
	{
		const TOptional<FVector> Direction = InGenSource ? InGenSource->GetDirection() : TOptional<FVector>();
		if (!Direction.IsSet())
		{
			return TOptional<FVector2D>();
		}

		FVector2D Yaw(Direction.GetValue());
		if (!Yaw.Normalize(UE_KINDA_SMALL_NUMBER))
		{
			return TOptional<FVector2D>();
		}

		return Yaw;
	}

	/** 1D interval overlap of two centered extents. */
	FORCEINLINE bool IntervalsOverlap(const double MinA, const double MaxA, const double MinB, const double MaxB)
	{
		return MinA <= MaxB && MinB <= MaxA;
	}

	/**
	 * Separating-axis test between the cell's XY rectangle and a yaw-rotated rectangle
	 * centered on the source. Axes: world X/Y (cell) + the rotated basis (box).
	 */
	bool YawBoxOverlapsCellXY(const FVector2D& InCenter, const FVector2D& InForward, const FVector2D& InHalfExtents, const FBox& InCellBounds)
	{
		const FVector2D Right(-InForward.Y, InForward.X);
		const FVector2D CellCenter((InCellBounds.Min.X + InCellBounds.Max.X) * 0.5, (InCellBounds.Min.Y + InCellBounds.Max.Y) * 0.5);
		const FVector2D CellExtents((InCellBounds.Max.X - InCellBounds.Min.X) * 0.5, (InCellBounds.Max.Y - InCellBounds.Min.Y) * 0.5);

		const FVector2D Axes[4] = {FVector2D(1.0, 0.0), FVector2D(0.0, 1.0), InForward, Right};

		for (const FVector2D& Axis : Axes)
		{
			const double CellProjection = FMath::Abs(Axis.X) * CellExtents.X + FMath::Abs(Axis.Y) * CellExtents.Y;
			const double BoxProjection = FMath::Abs(Axis | InForward) * InHalfExtents.X + FMath::Abs(Axis | Right) * InHalfExtents.Y;
			const double CenterDelta = FMath::Abs(Axis | (CellCenter - InCenter));

			if (CenterDelta > CellProjection + BoxProjection)
			{
				return false;
			}
		}

		return true;
	}

	/** True when all four XY corners of the cell lie inside a yaw-rotated rectangle. */
	bool CellCornersInsideYawRectXY(const FBox& InCell, const FVector2D& InCenter, const FVector2D& InForward, const FVector2D& InHalfExtents)
	{
		const FVector2D Right(-InForward.Y, InForward.X);
		const FVector2D Corners[4] = {
			FVector2D(InCell.Min.X, InCell.Min.Y), FVector2D(InCell.Max.X, InCell.Min.Y),
			FVector2D(InCell.Min.X, InCell.Max.Y), FVector2D(InCell.Max.X, InCell.Max.Y)};

		for (const FVector2D& Corner : Corners)
		{
			const FVector2D Delta = Corner - InCenter;
			if (FMath::Abs(Delta | InForward) > InHalfExtents.X || FMath::Abs(Delta | Right) > InHalfExtents.Y)
			{
				return false;
			}
		}

		return true;
	}

	/** True when the sphere fits entirely inside the cone (conservative near the boundary). */
	bool ConeContainsSphere(const FVector& InApex, const FVector& InAxis, const double InRange, const double InCosHalfAngle, const double InSinHalfAngle, const FVector& InSphereCenter, const double InSphereRadius)
	{
		const FVector ToCenter = InSphereCenter - InApex;

		// Cap: the whole sphere before the far plane.
		if ((ToCenter | InAxis) + InSphereRadius > InRange)
		{
			return false;
		}

		// Lateral: center inside the cone eroded by the sphere radius (apex shifted forward).
		const FVector ErodedApex = InApex + (InSphereRadius / InSinHalfAngle) * InAxis;
		const FVector FromErodedApex = InSphereCenter - ErodedApex;
		const double ErodedAxialDistance = FromErodedApex | InAxis;

		if (ErodedAxialDistance < 0.0)
		{
			return false;
		}

		return FMath::Square(ErodedAxialDistance) >= FMath::Square(InCosHalfAngle) * FromErodedApex.SizeSquared();
	}

	/**
	 * Conservative-inclusive sphere-vs-cone intersection (Eberly's expanded-cone test),
	 * with plane-capped apex/far checks padded by the sphere radius.
	 */
	bool ConeIntersectsSphere(const FVector& InApex, const FVector& InAxis, const double InRange, const double InCosHalfAngle, const double InSinHalfAngle, const FVector& InSphereCenter, const double InSphereRadius)
	{
		const FVector ToCenter = InSphereCenter - InApex;
		const double AxialDistance = ToCenter | InAxis;

		// Fully behind the apex plane, or fully beyond the far cap plane.
		if (AxialDistance < -InSphereRadius || AxialDistance - InSphereRadius > InRange)
		{
			return false;
		}

		// Spherical range cap around the apex.
		if (ToCenter.SizeSquared() > FMath::Square(InRange + InSphereRadius))
		{
			return false;
		}

		// Expanded-cone side test: shift the apex back along the axis by R / sin(θ);
		// the sphere intersects the cone iff its center lies inside that expanded cone.
		const FVector ExpandedApex = InApex - (InSphereRadius / InSinHalfAngle) * InAxis;
		const FVector FromExpandedApex = InSphereCenter - ExpandedApex;
		const double ExpandedAxialDistance = FromExpandedApex | InAxis;

		if (ExpandedAxialDistance < 0.0)
		{
			return false;
		}

		return FMath::Square(ExpandedAxialDistance) >= FMath::Square(InCosHalfAngle) * FromExpandedApex.SizeSquared();
	}
}

#pragma region UPCGExSchedulingConstraintSphere

bool UPCGExSchedulingConstraintSphere::EvaluateGate(const IPCGGenSourceBase* InGenSource, const FBox& InBounds, const bool bUse2DGrid, const bool bExpanded) const
{
	const TOptional<FVector> Position = InGenSource ? InGenSource->GetPosition() : TOptional<FVector>();
	if (!Position.IsSet())
	{
		// No position: never inside the shape.
		return bInvert;
	}

	const FVector SourcePosition = PCGExSchedulingShapes::SanitizedSourcePosition(Position.GetValue(), InBounds, bUse2DGrid);
	const double ScaledRadius = Radius * GetGateScale(bExpanded);
	bool bInside = InBounds.ComputeSquaredDistanceToPoint(SourcePosition) <= ScaledRadius * ScaledRadius;

	if (bInside && bHollow)
	{
		const double InnerRadius = FMath::Max(Radius - ShellThickness, 0.0) * GetInnerGateScale(bExpanded);
		if (InnerRadius > 0.0 && PCGExSchedulingShapes::MaxSquaredDistanceToPoint(InBounds, SourcePosition, bUse2DGrid) <= InnerRadius * InnerRadius)
		{
			// Cell fully inside the hollow interior.
			bInside = false;
		}
	}

	return bInside != bInvert;
}

TOptional<double> UPCGExSchedulingConstraintSphere::CalcPriority(const IPCGGenSourceBase* InGenSource, const FBox& InBounds, const bool bUse2DGrid) const
{
	const TOptional<FVector> Position = InGenSource ? InGenSource->GetPosition() : TOptional<FVector>();
	if (!Position.IsSet())
	{
		return TOptional<double>();
	}

	const FVector SourcePosition = PCGExSchedulingShapes::SanitizedSourcePosition(Position.GetValue(), InBounds, bUse2DGrid);
	const double Distance = FMath::Sqrt(InBounds.ComputeSquaredDistanceToPoint(SourcePosition));

	return ShapedPriority(1.0 - Distance / FMath::Max(Radius, 1.0));
}

#pragma endregion

#pragma region UPCGExSchedulingConstraintCylinder

bool UPCGExSchedulingConstraintCylinder::EvaluateGate(const IPCGGenSourceBase* InGenSource, const FBox& InBounds, const bool bUse2DGrid, const bool bExpanded) const
{
	const TOptional<FVector> Position = InGenSource ? InGenSource->GetPosition() : TOptional<FVector>();
	if (!Position.IsSet())
	{
		return bInvert;
	}

	const FVector SourcePosition = Position.GetValue();
	const double Scale = GetGateScale(bExpanded);

	const FBox2D BoundsXY(FVector2D(InBounds.Min), FVector2D(InBounds.Max));
	const double ScaledRadius = Radius * Scale;

	bool bInside = BoundsXY.ComputeSquaredDistanceToPoint(FVector2D(SourcePosition)) <= ScaledRadius * ScaledRadius;

	if (bInside && !bUse2DGrid)
	{
		const double ScaledHalfHeight = HalfHeight * Scale;
		bInside = (InBounds.Min.Z <= SourcePosition.Z + ScaledHalfHeight) && (InBounds.Max.Z >= SourcePosition.Z - ScaledHalfHeight);
	}

	if (bInside && bHollow)
	{
		const double InnerScale = GetInnerGateScale(bExpanded);
		const double InnerRadius = FMath::Max(Radius - ShellThickness, 0.0) * InnerScale;
		const double InnerHalfHeight = FMath::Max(HalfHeight - ShellThickness, 0.0) * InnerScale;

		if (InnerRadius > 0.0 && (bUse2DGrid || InnerHalfHeight > 0.0))
		{
			// Radial: farthest XY point of the cell within the inner radius.
			const bool bFullyInsideRadially = PCGExSchedulingShapes::MaxSquaredDistanceToPoint(InBounds, SourcePosition, /*bUse2DGrid=*/true) <= InnerRadius * InnerRadius;
			const bool bFullyInsideVertically = bUse2DGrid
				|| ((InBounds.Min.Z >= SourcePosition.Z - InnerHalfHeight) && (InBounds.Max.Z <= SourcePosition.Z + InnerHalfHeight));

			if (bFullyInsideRadially && bFullyInsideVertically)
			{
				bInside = false;
			}
		}
	}

	return bInside != bInvert;
}

TOptional<double> UPCGExSchedulingConstraintCylinder::CalcPriority(const IPCGGenSourceBase* InGenSource, const FBox& InBounds, const bool bUse2DGrid) const
{
	const TOptional<FVector> Position = InGenSource ? InGenSource->GetPosition() : TOptional<FVector>();
	if (!Position.IsSet())
	{
		return TOptional<double>();
	}

	// Radial closeness only — vertical position doesn't affect ordering.
	const FBox2D BoundsXY(FVector2D(InBounds.Min), FVector2D(InBounds.Max));
	const double RadialDistance = FMath::Sqrt(BoundsXY.ComputeSquaredDistanceToPoint(FVector2D(Position.GetValue())));

	return ShapedPriority(1.0 - RadialDistance / FMath::Max(Radius, 1.0));
}

#pragma endregion

#pragma region UPCGExSchedulingConstraintBox

bool UPCGExSchedulingConstraintBox::EvaluateGate(const IPCGGenSourceBase* InGenSource, const FBox& InBounds, const bool bUse2DGrid, const bool bExpanded) const
{
	const TOptional<FVector> Position = InGenSource ? InGenSource->GetPosition() : TOptional<FVector>();
	if (!Position.IsSet())
	{
		return bInvert;
	}

	const FVector SourcePosition = Position.GetValue();
	const FVector ScaledExtents = Extents * GetGateScale(bExpanded);

	TOptional<FVector2D> YawDirection;
	if (bAlignToSourceDirection)
	{
		YawDirection = PCGExSchedulingConstraintShapes::GetYawDirection(InGenSource);
	}

	bool bInside;

	if (YawDirection.IsSet())
	{
		bInside = PCGExSchedulingConstraintShapes::YawBoxOverlapsCellXY(
			FVector2D(SourcePosition), YawDirection.GetValue(), FVector2D(ScaledExtents.X, ScaledExtents.Y), InBounds);
	}
	else
	{
		// World-aligned fast path (also the fallback when the source has no usable yaw).
		bInside = PCGExSchedulingConstraintShapes::IntervalsOverlap(
				SourcePosition.X - ScaledExtents.X, SourcePosition.X + ScaledExtents.X, InBounds.Min.X, InBounds.Max.X)
			&& PCGExSchedulingConstraintShapes::IntervalsOverlap(
				SourcePosition.Y - ScaledExtents.Y, SourcePosition.Y + ScaledExtents.Y, InBounds.Min.Y, InBounds.Max.Y);
	}

	if (bInside && !bUse2DGrid)
	{
		bInside = PCGExSchedulingConstraintShapes::IntervalsOverlap(
			SourcePosition.Z - ScaledExtents.Z, SourcePosition.Z + ScaledExtents.Z, InBounds.Min.Z, InBounds.Max.Z);
	}

	if (bInside && bHollow)
	{
		const double InnerScale = GetInnerGateScale(bExpanded);
		const FVector InnerExtents(
			FMath::Max(Extents.X - ShellThickness, 0.0) * InnerScale,
			FMath::Max(Extents.Y - ShellThickness, 0.0) * InnerScale,
			FMath::Max(Extents.Z - ShellThickness, 0.0) * InnerScale);

		// The erosion may consume an axis entirely — then there is no interior and the shell is the whole shape.
		if (InnerExtents.X > 0.0 && InnerExtents.Y > 0.0 && (bUse2DGrid || InnerExtents.Z > 0.0))
		{
			bool bFullyInside;

			if (YawDirection.IsSet())
			{
				bFullyInside = PCGExSchedulingConstraintShapes::CellCornersInsideYawRectXY(
					InBounds, FVector2D(SourcePosition), YawDirection.GetValue(), FVector2D(InnerExtents.X, InnerExtents.Y));
			}
			else
			{
				bFullyInside =
					InBounds.Min.X >= SourcePosition.X - InnerExtents.X && InBounds.Max.X <= SourcePosition.X + InnerExtents.X
					&& InBounds.Min.Y >= SourcePosition.Y - InnerExtents.Y && InBounds.Max.Y <= SourcePosition.Y + InnerExtents.Y;
			}

			if (bFullyInside && !bUse2DGrid)
			{
				bFullyInside = InBounds.Min.Z >= SourcePosition.Z - InnerExtents.Z && InBounds.Max.Z <= SourcePosition.Z + InnerExtents.Z;
			}

			if (bFullyInside)
			{
				bInside = false;
			}
		}
	}

	return bInside != bInvert;
}

TOptional<double> UPCGExSchedulingConstraintBox::CalcPriority(const IPCGGenSourceBase* InGenSource, const FBox& InBounds, const bool bUse2DGrid) const
{
	const TOptional<FVector> Position = InGenSource ? InGenSource->GetPosition() : TOptional<FVector>();
	if (!Position.IsSet())
	{
		return TOptional<double>();
	}

	const FVector SourcePosition = PCGExSchedulingShapes::SanitizedSourcePosition(Position.GetValue(), InBounds, bUse2DGrid);
	FVector Delta = InBounds.GetClosestPointTo(SourcePosition) - SourcePosition;

	// Express the offset in the box frame when yaw-aligned so closeness follows the shape.
	if (bAlignToSourceDirection)
	{
		if (const TOptional<FVector2D> YawDirection = PCGExSchedulingConstraintShapes::GetYawDirection(InGenSource); YawDirection.IsSet())
		{
			const FVector2D Forward = YawDirection.GetValue();
			const FVector2D Right(-Forward.Y, Forward.X);
			const FVector2D DeltaXY(Delta);
			Delta = FVector(DeltaXY | Forward, DeltaXY | Right, Delta.Z);
		}
	}

	// Chebyshev-style closeness: 0 at the box surface, 1 at the source.
	double MaxAxisRatio = FMath::Max(FMath::Abs(Delta.X) / FMath::Max(Extents.X, 1.0), FMath::Abs(Delta.Y) / FMath::Max(Extents.Y, 1.0));
	if (!bUse2DGrid)
	{
		MaxAxisRatio = FMath::Max(MaxAxisRatio, FMath::Abs(Delta.Z) / FMath::Max(Extents.Z, 1.0));
	}

	return ShapedPriority(1.0 - MaxAxisRatio);
}

#pragma endregion

#pragma region UPCGExSchedulingConstraintCone

bool UPCGExSchedulingConstraintCone::EvaluateGate(const IPCGGenSourceBase* InGenSource, const FBox& InBounds, const bool bUse2DGrid, const bool bExpanded) const
{
	const TOptional<FVector> Position = InGenSource ? InGenSource->GetPosition() : TOptional<FVector>();
	if (!Position.IsSet())
	{
		return bInvert;
	}

	const double Scale = GetGateScale(bExpanded);
	const double ScaledRange = Range * Scale;

	FVector Apex = Position.GetValue();
	FVector Axis = FVector::ZeroVector;

	const TOptional<FVector> Direction = InGenSource->GetDirection();
	if (Direction.IsSet())
	{
		Axis = Direction.GetValue();
		if (bUse2DGrid)
		{
			Axis.Z = 0.0;
		}

		if (!Axis.Normalize(UE_KINDA_SMALL_NUMBER))
		{
			Axis = FVector::ZeroVector;
		}
	}

	if (bUse2DGrid)
	{
		Apex.Z = InBounds.Min.Z;
	}

	bool bInside;

	if (Axis.IsZero())
	{
		// No usable direction: fall back to a sphere of Range (hollow becomes a spherical shell).
		bInside = InBounds.ComputeSquaredDistanceToPoint(Apex) <= ScaledRange * ScaledRange;

		if (bInside && bHollow)
		{
			const double InnerRadius = FMath::Max(Range - ShellThickness, 0.0) * GetInnerGateScale(bExpanded);
			if (InnerRadius > 0.0 && PCGExSchedulingShapes::MaxSquaredDistanceToPoint(InBounds, Apex, bUse2DGrid) <= InnerRadius * InnerRadius)
			{
				bInside = false;
			}
		}
	}
	else
	{
		// Cell bounding sphere, flattened onto the cell plane for 2D grids.
		FVector SphereCenter = InBounds.GetCenter();
		FVector SphereExtents = InBounds.GetExtent();
		if (bUse2DGrid)
		{
			SphereCenter.Z = InBounds.Min.Z;
			SphereExtents.Z = 0.0;
		}

		const double HalfAngleRadians = FMath::DegreesToRadians(FMath::Clamp(HalfAngleDegrees, 1.0, 89.0));
		const double CosHalfAngle = FMath::Cos(HalfAngleRadians);
		const double SinHalfAngle = FMath::Sin(HalfAngleRadians);
		const double SphereRadius = SphereExtents.Size();

		bInside = PCGExSchedulingConstraintShapes::ConeIntersectsSphere(
			Apex, Axis, ScaledRange, CosHalfAngle, SinHalfAngle, SphereCenter, SphereRadius * Scale);

		if (bInside && bHollow)
		{
			// Inner cone = the cone eroded by the shell thickness (apex shifts forward by T/sin,
			// range shrinks by T), then scaled by the mirrored hysteresis factor.
			const double InnerScale = GetInnerGateScale(bExpanded);
			const double InnerRange = FMath::Max(Range - ShellThickness, 0.0) * InnerScale;

			if (InnerRange > 0.0)
			{
				const FVector InnerApex = Apex + Axis * ((ShellThickness / SinHalfAngle) * InnerScale);

				if (PCGExSchedulingConstraintShapes::ConeContainsSphere(
					InnerApex, Axis, InnerRange, CosHalfAngle, SinHalfAngle, SphereCenter, SphereRadius))
				{
					bInside = false;
				}
			}
		}
	}

	return bInside != bInvert;
}

TOptional<double> UPCGExSchedulingConstraintCone::CalcPriority(const IPCGGenSourceBase* InGenSource, const FBox& InBounds, const bool bUse2DGrid) const
{
	const TOptional<FVector> Position = InGenSource ? InGenSource->GetPosition() : TOptional<FVector>();
	if (!Position.IsSet())
	{
		return TOptional<double>();
	}

	// Radial closeness within Range; pair with a Direction Alignment constraint for angular priority.
	const FVector SourcePosition = PCGExSchedulingShapes::SanitizedSourcePosition(Position.GetValue(), InBounds, bUse2DGrid);
	const double Distance = FMath::Sqrt(InBounds.ComputeSquaredDistanceToPoint(SourcePosition));

	return ShapedPriority(1.0 - Distance / FMath::Max(Range, 1.0));
}

#pragma endregion
