// Copyright 2026 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#include "Constraints/PCGExSchedulingConstraintTargets.h"

#include "PCGExSchedulingCommon.h"

#include "DrawDebugHelpers.h"
#include "EngineDefines.h"
#include "GameFramework/Volume.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(PCGExSchedulingConstraintTargets)

namespace PCGExSchedulingConstraintTargets
{
	bool BoxOverlap(const FBox& InRegion, const FBox& InCell, const bool bUse2DGrid)
	{
		if (!PCGExScheduling::IntervalsOverlap(InRegion.Min.X, InRegion.Max.X, InCell.Min.X, InCell.Max.X)
			|| !PCGExScheduling::IntervalsOverlap(InRegion.Min.Y, InRegion.Max.Y, InCell.Min.Y, InCell.Max.Y))
		{
			return false;
		}

		return bUse2DGrid || PCGExScheduling::IntervalsOverlap(InRegion.Min.Z, InRegion.Max.Z, InCell.Min.Z, InCell.Max.Z);
	}

	double BoxToBoxDistance(const FBox& InRegion, const FBox& InCell, const bool bUse2DGrid)
	{
		if (!bUse2DGrid)
		{
			return FMath::Sqrt(InRegion.ComputeSquaredDistanceToBox(InCell));
		}

		const double GapX = FMath::Max3(0.0, InRegion.Min.X - InCell.Max.X, InCell.Min.X - InRegion.Max.X);
		const double GapY = FMath::Max3(0.0, InRegion.Min.Y - InCell.Max.Y, InCell.Min.Y - InRegion.Max.Y);

		return FMath::Sqrt(GapX * GapX + GapY * GapY);
	}

	/**
	 * Min squared distance from a point to the shape's polyline (XY only on 2D grids). Stops at the
	 * first segment at or below InStopBelowSquared -- pass 0 for the exact minimum.
	 */
	double PolylineDistanceSquared(const FPCGExTargetShape& InShape, const FVector& InPoint, const bool bUse2DGrid, const double InStopBelowSquared)
	{
		const TArray<FVector>& Points = InShape.SplinePoints;
		const int32 NumSegments = InShape.NumSegments();

		double BestDistanceSquared = TNumericLimits<double>::Max();

		if (bUse2DGrid)
		{
			const FVector2D Point(InPoint);

			for (int32 Index = 0; Index < NumSegments; ++Index)
			{
				const FVector2D ClosestPoint = FMath::ClosestPointOnSegment2D(Point, FVector2D(Points[Index]), FVector2D(Points[InShape.NextPointIndex(Index)]));
				BestDistanceSquared = FMath::Min(BestDistanceSquared, FVector2D::DistSquared(Point, ClosestPoint));

				if (BestDistanceSquared <= InStopBelowSquared) { break; }
			}
		}
		else
		{
			for (int32 Index = 0; Index < NumSegments; ++Index)
			{
				const FVector ClosestPoint = FMath::ClosestPointOnSegment(InPoint, Points[Index], Points[InShape.NextPointIndex(Index)]);
				BestDistanceSquared = FMath::Min(BestDistanceSquared, FVector::DistSquared(InPoint, ClosestPoint));

				if (BestDistanceSquared <= InStopBelowSquared) { break; }
			}
		}

		return BestDistanceSquared;
	}

	/** Conservative cell circumradius (2D-flattened when asked). */
	double CellCircumradius(const FBox& InCell, const bool bUse2DGrid)
	{
		FVector Extents = InCell.GetExtent();
		if (bUse2DGrid) { Extents.Z = 0.0; }
		return Extents.Size();
	}

	/** XY point-in-polygon (crossing number) over a closed polyline. */
	bool PointInPolygonXY(const FVector& InPoint, const TArray<FVector>& InPoints)
	{
		bool bInside = false;
		const int32 NumPoints = InPoints.Num();

		for (int32 Index = 0, PrevIndex = NumPoints - 1; Index < NumPoints; PrevIndex = Index++)
		{
			const FVector& A = InPoints[Index];
			const FVector& B = InPoints[PrevIndex];

			if ((A.Y > InPoint.Y) != (B.Y > InPoint.Y))
			{
				const double CrossingX = (B.X - A.X) * (InPoint.Y - A.Y) / (B.Y - A.Y) + A.X;
				if (InPoint.X < CrossingX)
				{
					bInside = !bInside;
				}
			}
		}

		return bInside;
	}

	/** Closed-spline interior test: XY polygon fill, vertical reach = the polyline's Z range plus the corridor radius. */
	bool InsideFilledSpline(const FPCGExTargetShape& InShape, const FVector& InCellCenter, const double InCorridorRadius, const bool bUse2DGrid)
	{
		if (!InShape.bClosedSpline || InShape.SplinePoints.Num() < 3)
		{
			return false;
		}

		if (!bUse2DGrid
			&& (InCellCenter.Z < InShape.Bounds.Min.Z - InCorridorRadius || InCellCenter.Z > InShape.Bounds.Max.Z + InCorridorRadius))
		{
			return false;
		}

		return PointInPolygonXY(InCellCenter, InShape.SplinePoints);
	}

#if UE_ENABLE_DEBUG_DRAWING
	void DrawRegion(const UWorld* InWorld, const FBox& InRegion, const FColor& InColor)
	{
		DrawDebugBox(InWorld, InRegion.GetCenter(), InRegion.GetExtent(), InColor, /*bPersistentLines=*/false, /*LifeTime=*/0.0f, /*DepthPriority=*/0, /*Thickness=*/0.0f);
	}
#endif
}

#pragma region UPCGExSchedulingConstraintTargetsBase

bool UPCGExSchedulingConstraintTargetsBase::EvaluateGate(const IPCGGenSourceBase* InGenSource, const FBox& InBounds, const bool bUse2DGrid, const bool bExpanded) const
{
	// Purely world-region driven -- the generation source only matters through the engine's radius broadphase.
	// Cleanup (bExpanded) is concurrent and read-only; the scan refreshes.
	const FPCGExTargetSnapshot* Snapshot = bExpanded ? GetCachedSnapshot() : RefreshTargetSnapshot();

	bool bInside = false;

	if (Snapshot)
	{
		const TArray<FPCGExTargetRegion>& Regions = GetCachedRegions();
		checkSlow(Regions.Num() == Snapshot->Shapes.Num());

		FPCGExTargetTestContext Context;
		Context.bExpanded = bExpanded;
		Context.bUse2DGrid = bUse2DGrid;
		Context.bGameThread = IsInGameThread();

		for (int32 Index = 0; Index < Snapshot->Shapes.Num(); ++Index)
		{
			if (TestShape(Snapshot->Shapes[Index], Regions[Index].Get(bExpanded), InBounds, Context))
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

	const FPCGExTargetSnapshot* Snapshot = RefreshTargetSnapshot();
	if (!Snapshot || Snapshot->Shapes.IsEmpty())
	{
		return TOptional<double>();
	}

	const TArray<FPCGExTargetRegion>& Regions = GetCachedRegions();

	double MinDistance = TNumericLimits<double>::Max();
	for (int32 Index = 0; Index < Snapshot->Shapes.Num(); ++Index)
	{
		MinDistance = FMath::Min(MinDistance, DistanceToShape(Snapshot->Shapes[Index], Regions[Index].Generate, InBounds, bUse2DGrid));
	}

	double Closeness = 1.0 - FMath::Clamp(MinDistance / PriorityFalloffDistance, 0.0, 1.0);
	if (bInvert)
	{
		Closeness = 1.0 - Closeness;
	}

	return FMath::Pow(Closeness, FalloffExponent);
}

void UPCGExSchedulingConstraintTargetsBase::DebugDraw(const UWorld* InWorld, const IPCGGenSourceBase* InGenSource, const bool bUse2DGrid, const bool bDrawCleanup) const
{
#if UE_ENABLE_DEBUG_DRAWING
	const FPCGExTargetSnapshot* Snapshot = RefreshTargetSnapshot();
	if (!Snapshot)
	{
		return;
	}

	const TArray<FPCGExTargetRegion>& Regions = GetCachedRegions();
	const FColor GateColor = PCGExSchedulingDebug::GateColor(bInvert);

	for (int32 Index = 0; Index < Snapshot->Shapes.Num(); ++Index)
	{
		const FPCGExTargetShape& Shape = Snapshot->Shapes[Index];

		PCGExSchedulingConstraintTargets::DrawRegion(InWorld, Regions[Index].Generate, GateColor);

		if (bDrawCleanup)
		{
			PCGExSchedulingConstraintTargets::DrawRegion(InWorld, Regions[Index].Cleanup, PCGExSchedulingDebug::CleanupColor());
		}

		const int32 NumSegments = Shape.NumSegments();
		for (int32 SegmentIndex = 0; SegmentIndex < NumSegments; ++SegmentIndex)
		{
			DrawDebugLine(InWorld, Shape.SplinePoints[SegmentIndex], Shape.SplinePoints[Shape.NextPointIndex(SegmentIndex)], GateColor, /*bPersistentLines=*/false, /*LifeTime=*/0.0f, /*DepthPriority=*/0, /*Thickness=*/2.0f);
		}
	}
#endif
}

double UPCGExSchedulingConstraintTargetsBase::DistanceToShape(const FPCGExTargetShape& InShape, const FBox& InGenerateRegion, const FBox& InBounds, const bool bUse2DGrid) const
{
	return PCGExSchedulingConstraintTargets::BoxToBoxDistance(InGenerateRegion, InBounds, bUse2DGrid);
}

#if WITH_EDITOR
void UPCGExSchedulingConstraintTargetsBase::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);

	// The query (and so the cache key) may have changed -- re-acquire on the next scan.
	ReleaseTargetSlot();
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
	Query.TrackingInterval = TrackingInterval;
	Query.bRefreshConsumers = bRefreshOnTargetChange;

	return Query;
}

const FPCGExTargetSnapshot* UPCGExSchedulingConstraintTargetsBase::RefreshTargetSnapshot() const
{
	check(IsInGameThread());

	// The subsystem that issued the slot is gone (world torn down, entry released) -- start over.
	if (CachedSlot && CachedSlot->bOrphaned)
	{
		CachedSlot.Reset();
	}

	if (!CachedSlot)
	{
		UPCGExSchedulingSubsystem* Subsystem = UPCGExSchedulingSubsystem::GetInstance(GetWorld());
		if (Subsystem)
		{
			AcquiredQuery = BuildTargetQuery();
			CachedSlot = Subsystem->AcquireTargetSlot(AcquiredQuery, this);
		}

		if (!CachedSlot)
		{
			CachedSnapshot.Reset();
			CachedRegions.Reset();
			return nullptr;
		}
	}

	// Padding is read live so runtime edits of expansion / hysteresis / invert take effect on the next scan.
	const double RegionExpansion = GetBoundsExpansion();
	const double CleanupScale = GetRegionScale(/*bExpanded=*/true);

	if (CachedSnapshot != CachedSlot->Snapshot || CachedRegionExpansion != RegionExpansion || CachedCleanupScale != CleanupScale)
	{
		CachedSnapshot = CachedSlot->Snapshot;
		CachedRegionExpansion = RegionExpansion;
		CachedCleanupScale = CleanupScale;
		BakeRegions();
	}

	return CachedSnapshot.Get();
}

void UPCGExSchedulingConstraintTargetsBase::BakeRegions() const
{
	CachedRegions.Reset();

	if (!CachedSnapshot)
	{
		return;
	}

	CachedRegions.Reserve(CachedSnapshot->Shapes.Num());

	for (const FPCGExTargetShape& Shape : CachedSnapshot->Shapes)
	{
		FPCGExTargetRegion& Region = CachedRegions.Emplace_GetRef();
		Region.Generate = Shape.Bounds.ExpandBy(CachedRegionExpansion);
		Region.Cleanup = FBox::BuildAABB(Region.Generate.GetCenter(), Region.Generate.GetExtent() * CachedCleanupScale);
	}
}

void UPCGExSchedulingConstraintTargetsBase::ReleaseTargetSlot() const
{
	check(IsInGameThread());

	if (!CachedSlot)
	{
		return;
	}

	if (UPCGExSchedulingSubsystem* Subsystem = UPCGExSchedulingSubsystem::GetInstance(GetWorld()))
	{
		Subsystem->ReleaseTargetSlot(AcquiredQuery, this);
	}

	CachedSlot.Reset();
	CachedSnapshot.Reset();
	CachedRegions.Reset();
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

bool UPCGExSchedulingConstraintTargetBounds::TestShape(const FPCGExTargetShape& InShape, const FBox& InRegion, const FBox& InBounds, const FPCGExTargetTestContext& InContext) const
{
	return PCGExSchedulingConstraintTargets::BoxOverlap(InRegion, InBounds, InContext.bUse2DGrid);
}

#pragma endregion

#pragma region UPCGExSchedulingConstraintTargetVolume

bool UPCGExSchedulingConstraintTargetVolume::TestShape(const FPCGExTargetShape& InShape, const FBox& InRegion, const FBox& InBounds, const FPCGExTargetTestContext& InContext) const
{
	if (!PCGExSchedulingConstraintTargets::BoxOverlap(InRegion, InBounds, InContext.bUse2DGrid))
	{
		return false;
	}

	// Precise brush test: generation path only (game thread, non-expanded, 3D) -- cell
	// center + circumradius, conservative-inclusive against the actual brush. Skipped when
	// inverted: narrowing only the generate region while cleanup keeps the box would break
	// the inverted keep⊇generate hysteresis invariant (generate↔cull flicker).
	if (bPreciseVolumeTest && !bInvert && InContext.bGameThread && !InContext.bExpanded && !InContext.bUse2DGrid)
	{
		if (const AVolume* Volume = InShape.Volume.Get())
		{
			return Volume->EncompassesPoint(InBounds.GetCenter(), static_cast<float>(InBounds.GetExtent().Size()));
		}
	}

	return true;
}

#pragma endregion

#pragma region UPCGExSchedulingConstraintTargetSpline

bool UPCGExSchedulingConstraintTargetSpline::TestShape(const FPCGExTargetShape& InShape, const FBox& InRegion, const FBox& InBounds, const FPCGExTargetTestContext& InContext) const
{
	// Broadphase against the polyline bounds inflated by the corridor.
	if (!PCGExSchedulingConstraintTargets::BoxOverlap(InRegion, InBounds, InContext.bUse2DGrid))
	{
		return false;
	}

	// Actors without usable splines fall back to their bounds (broadphase already passed).
	if (InShape.SplinePoints.Num() < 2)
	{
		return true;
	}

	// Conservative-inclusive: distance from the cell center to the polyline vs corridor + cell
	// circumradius. The walk stops at the first segment within the threshold.
	const double Radius = SplineRadius * GetRegionScale(InContext.bExpanded);
	const FVector CellCenter = InBounds.GetCenter();
	const double Threshold = Radius + PCGExSchedulingConstraintTargets::CellCircumradius(InBounds, InContext.bUse2DGrid);
	const double ThresholdSquared = Threshold * Threshold;

	if (PCGExSchedulingConstraintTargets::PolylineDistanceSquared(InShape, CellCenter, InContext.bUse2DGrid, ThresholdSquared) <= ThresholdSquared)
	{
		return true;
	}

	// Interior fill for closed splines. Membership deep inside is scale-independent --
	// the corridor (scaled) provides the hysteresis band at the polygon boundary.
	return bFillClosedSplines && PCGExSchedulingConstraintTargets::InsideFilledSpline(InShape, CellCenter, Radius, InContext.bUse2DGrid);
}

double UPCGExSchedulingConstraintTargetSpline::DistanceToShape(const FPCGExTargetShape& InShape, const FBox& InGenerateRegion, const FBox& InBounds, const bool bUse2DGrid) const
{
	if (InShape.SplinePoints.Num() < 2)
	{
		return PCGExSchedulingConstraintTargets::BoxToBoxDistance(InGenerateRegion, InBounds, bUse2DGrid);
	}

	const FVector CellCenter = InBounds.GetCenter();

	if (bFillClosedSplines && PCGExSchedulingConstraintTargets::InsideFilledSpline(InShape, CellCenter, SplineRadius, bUse2DGrid))
	{
		return 0.0;
	}

	// Distance to the corridor surface (0 inside). Exact minimum: no early stop.
	const double Distance = FMath::Sqrt(PCGExSchedulingConstraintTargets::PolylineDistanceSquared(InShape, CellCenter, bUse2DGrid, 0.0)) - SplineRadius;
	return FMath::Max(Distance, 0.0);
}

#pragma endregion
