// Copyright 2026 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#include "Constraints/PCGExSchedulingConstraintTargets.h"

#include "GameFramework/Volume.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(PCGExSchedulingConstraintTargets)

namespace PCGExSchedulingConstraintTargets
{
	FORCEINLINE bool IntervalsOverlap(const double MinA, const double MaxA, const double MinB, const double MaxB)
	{
		return MinA <= MaxB && MinB <= MaxA;
	}

	/** Region bounds expanded by a flat amount, then scaled about their center (hysteresis). */
	FBox ScaledRegion(const FBox& InRegion, const double InExpansion, const double InScale)
	{
		const FBox Expanded = InRegion.ExpandBy(InExpansion);
		const FVector Center = Expanded.GetCenter();
		const FVector Extents = Expanded.GetExtent() * InScale;
		return FBox(Center - Extents, Center + Extents);
	}

	bool BoxOverlap(const FBox& InRegion, const FBox& InCell, const bool bUse2DGrid)
	{
		if (!IntervalsOverlap(InRegion.Min.X, InRegion.Max.X, InCell.Min.X, InCell.Max.X)
			|| !IntervalsOverlap(InRegion.Min.Y, InRegion.Max.Y, InCell.Min.Y, InCell.Max.Y))
		{
			return false;
		}

		return bUse2DGrid || IntervalsOverlap(InRegion.Min.Z, InRegion.Max.Z, InCell.Min.Z, InCell.Max.Z);
	}

	double BoxToBoxDistance(const FBox& InRegion, const FBox& InCell, const bool bUse2DGrid)
	{
		const double GapX = FMath::Max3(0.0, InRegion.Min.X - InCell.Max.X, InCell.Min.X - InRegion.Max.X);
		const double GapY = FMath::Max3(0.0, InRegion.Min.Y - InCell.Max.Y, InCell.Min.Y - InRegion.Max.Y);
		const double GapZ = bUse2DGrid ? 0.0 : FMath::Max3(0.0, InRegion.Min.Z - InCell.Max.Z, InCell.Min.Z - InRegion.Max.Z);

		return FMath::Sqrt(GapX * GapX + GapY * GapY + GapZ * GapZ);
	}

	/** Min distance from the cell center to the shape's polyline (2D-flattened when asked). */
	double PolylineDistance(const FPCGExTargetShape& InShape, const FBox& InCell, const bool bUse2DGrid)
	{
		FVector Center = InCell.GetCenter();
		if (bUse2DGrid) { Center.Z = 0.0; }

		const int32 NumPoints = InShape.SplinePoints.Num();
		const int32 NumSegments = InShape.bClosedSpline ? NumPoints : NumPoints - 1;

		double BestDistanceSquared = TNumericLimits<double>::Max();

		for (int32 Index = 0; Index < NumSegments; ++Index)
		{
			FVector SegmentStart = InShape.SplinePoints[Index];
			FVector SegmentEnd = InShape.SplinePoints[(Index + 1) % NumPoints];

			if (bUse2DGrid)
			{
				SegmentStart.Z = 0.0;
				SegmentEnd.Z = 0.0;
			}

			const FVector ClosestPoint = FMath::ClosestPointOnSegment(Center, SegmentStart, SegmentEnd);
			BestDistanceSquared = FMath::Min(BestDistanceSquared, FVector::DistSquared(Center, ClosestPoint));
		}

		return NumSegments > 0 ? FMath::Sqrt(BestDistanceSquared) : TNumericLimits<double>::Max();
	}

	/** Conservative cell circumradius (2D-flattened when asked). */
	double CellCircumradius(const FBox& InCell, const bool bUse2DGrid)
	{
		FVector Extents = InCell.GetExtent();
		if (bUse2DGrid) { Extents.Z = 0.0; }
		return Extents.Size();
	}
}

#pragma region UPCGExSchedulingConstraintTargetsBase

bool UPCGExSchedulingConstraintTargetsBase::EvaluateGate(const IPCGGenSourceBase* InGenSource, const FBox& InBounds, const bool bUse2DGrid, const bool bExpanded) const
{
	// Purely world-region driven — the generation source only matters through the engine's radius broadphase.
	const TSharedPtr<const FPCGExTargetSnapshot> Snapshot = GetTargetSnapshot();

	bool bInside = false;

	if (Snapshot)
	{
		FPCGExTargetTestContext Context;
		Context.Scale = GetRegionScale(bExpanded);
		Context.bExpanded = bExpanded;
		Context.bUse2DGrid = bUse2DGrid;
		Context.bGameThread = IsInGameThread();

		for (const FPCGExTargetShape& Shape : Snapshot->Shapes)
		{
			if (TestShape(Shape, InBounds, Context))
			{
				bInside = true;
				break;
			}
		}
	}

	return bInside != bInvert;
}

TOptional<double> UPCGExSchedulingConstraintTargetsBase::CalcPriority(const IPCGGenSourceBase* InGenSource, const FBox& InBounds, const bool bUse2DGrid) const
{
	if (PriorityFalloffDistance <= 0.0f)
	{
		return TOptional<double>();
	}

	const TSharedPtr<const FPCGExTargetSnapshot> Snapshot = GetTargetSnapshot();
	if (!Snapshot || Snapshot->Shapes.IsEmpty())
	{
		return TOptional<double>();
	}

	double MinDistance = TNumericLimits<double>::Max();
	for (const FPCGExTargetShape& Shape : Snapshot->Shapes)
	{
		MinDistance = FMath::Min(MinDistance, DistanceToShape(Shape, InBounds, bUse2DGrid));
	}

	double Closeness = 1.0 - FMath::Clamp(MinDistance / PriorityFalloffDistance, 0.0, 1.0);
	if (bInvert)
	{
		Closeness = 1.0 - Closeness;
	}

	return FMath::Pow(Closeness, FalloffExponent);
}

#if WITH_EDITOR
void UPCGExSchedulingConstraintTargetsBase::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);

	if (UPCGExSchedulingSubsystem* Subsystem = UPCGExSchedulingSubsystem::GetInstance(GetWorld()))
	{
		Subsystem->InvalidateTargetCache(this);
	}
}
#endif

FPCGExTargetQuery UPCGExSchedulingConstraintTargetsBase::BuildTargetQuery() const
{
	FPCGExTargetQuery Query;

	Query.TargetActors.Reserve(TargetActors.Num());
	for (const TSoftObjectPtr<AActor>& Target : TargetActors)
	{
		if (!Target.IsNull())
		{
			Query.TargetActors.Add(Target.ToSoftObjectPath());
		}
	}

	Query.TargetTag = bQueryByTag ? TargetTag : NAME_None;
	Query.TagQueryInterval = TagQueryInterval;
	Query.Geometry = GetTargetGeometry();
	Query.SplineErrorTolerance = GetSplineErrorTolerance();
	Query.bTrackMovement = bTrackTargetMovement;
	Query.MovementTolerance = MovementTolerance;

	return Query;
}

TSharedPtr<const FPCGExTargetSnapshot> UPCGExSchedulingConstraintTargetsBase::GetTargetSnapshot() const
{
	UPCGExSchedulingSubsystem* Subsystem = UPCGExSchedulingSubsystem::GetInstance(GetWorld());
	if (!Subsystem)
	{
		return nullptr;
	}

	if (IsInGameThread())
	{
		return Subsystem->ResolveTargetSnapshot(this, [this] { return BuildTargetQuery(); }, this);
	}

	return Subsystem->GetCachedTargetSnapshot(this);
}

double UPCGExSchedulingConstraintTargetsBase::GetRegionScale(const bool bExpanded) const
{
	if (!bExpanded)
	{
		return 1.0;
	}

	const double Scale = FMath::Max(static_cast<double>(CleanupExpansionScale), 1.0);
	return bInvert ? 1.0 / Scale : Scale;
}

#pragma endregion

#pragma region UPCGExSchedulingConstraintTargetBounds

bool UPCGExSchedulingConstraintTargetBounds::TestShape(const FPCGExTargetShape& InShape, const FBox& InBounds, const FPCGExTargetTestContext& InContext) const
{
	const FBox Region = PCGExSchedulingConstraintTargets::ScaledRegion(InShape.Bounds, BoundsExpansion, InContext.Scale);
	return PCGExSchedulingConstraintTargets::BoxOverlap(Region, InBounds, InContext.bUse2DGrid);
}

double UPCGExSchedulingConstraintTargetBounds::DistanceToShape(const FPCGExTargetShape& InShape, const FBox& InBounds, const bool bUse2DGrid) const
{
	return PCGExSchedulingConstraintTargets::BoxToBoxDistance(InShape.Bounds.ExpandBy(BoundsExpansion), InBounds, bUse2DGrid);
}

#pragma endregion

#pragma region UPCGExSchedulingConstraintTargetVolume

bool UPCGExSchedulingConstraintTargetVolume::TestShape(const FPCGExTargetShape& InShape, const FBox& InBounds, const FPCGExTargetTestContext& InContext) const
{
	const FBox Region = PCGExSchedulingConstraintTargets::ScaledRegion(InShape.Bounds, BoundsExpansion, InContext.Scale);
	if (!PCGExSchedulingConstraintTargets::BoxOverlap(Region, InBounds, InContext.bUse2DGrid))
	{
		return false;
	}

	// Precise brush test: generation path only (game thread, non-expanded, 3D) — cell
	// center + circumradius, conservative-inclusive against the actual brush.
	if (bPreciseVolumeTest && InContext.bGameThread && !InContext.bExpanded && !InContext.bUse2DGrid)
	{
		if (const AVolume* Volume = InShape.Volume.Get())
		{
			return Volume->EncompassesPoint(InBounds.GetCenter(), static_cast<float>(InBounds.GetExtent().Size()));
		}
	}

	return true;
}

double UPCGExSchedulingConstraintTargetVolume::DistanceToShape(const FPCGExTargetShape& InShape, const FBox& InBounds, const bool bUse2DGrid) const
{
	return PCGExSchedulingConstraintTargets::BoxToBoxDistance(InShape.Bounds.ExpandBy(BoundsExpansion), InBounds, bUse2DGrid);
}

#pragma endregion

#pragma region UPCGExSchedulingConstraintTargetSpline

bool UPCGExSchedulingConstraintTargetSpline::TestShape(const FPCGExTargetShape& InShape, const FBox& InBounds, const FPCGExTargetTestContext& InContext) const
{
	const double Radius = SplineRadius * InContext.Scale;

	// Broadphase against the polyline bounds inflated by the corridor.
	const FBox Region = PCGExSchedulingConstraintTargets::ScaledRegion(InShape.Bounds, SplineRadius, InContext.Scale);
	if (!PCGExSchedulingConstraintTargets::BoxOverlap(Region, InBounds, InContext.bUse2DGrid))
	{
		return false;
	}

	// Actors without usable splines fall back to their bounds (broadphase already passed).
	if (InShape.SplinePoints.Num() < 2)
	{
		return true;
	}

	// Conservative-inclusive: distance from the cell center to the polyline vs corridor + cell circumradius.
	const double Threshold = Radius + PCGExSchedulingConstraintTargets::CellCircumradius(InBounds, InContext.bUse2DGrid);
	return PCGExSchedulingConstraintTargets::PolylineDistance(InShape, InBounds, InContext.bUse2DGrid) <= Threshold;
}

double UPCGExSchedulingConstraintTargetSpline::DistanceToShape(const FPCGExTargetShape& InShape, const FBox& InBounds, const bool bUse2DGrid) const
{
	if (InShape.SplinePoints.Num() < 2)
	{
		return PCGExSchedulingConstraintTargets::BoxToBoxDistance(InShape.Bounds.ExpandBy(SplineRadius), InBounds, bUse2DGrid);
	}

	// Distance to the corridor surface (0 inside).
	const double Distance = PCGExSchedulingConstraintTargets::PolylineDistance(InShape, InBounds, bUse2DGrid) - SplineRadius;
	return FMath::Max(Distance, 0.0);
}

#pragma endregion
