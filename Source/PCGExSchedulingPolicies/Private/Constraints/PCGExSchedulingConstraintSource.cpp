// Copyright 2026 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#include "Constraints/PCGExSchedulingConstraintSource.h"

#include "Constraints/PCGExSchedulingConstraintShapes.h"
#include "RuntimeGen/GenSources/PCGGenSourceBase.h"

#include "ConvexVolume.h"
#include "DrawDebugHelpers.h"
#include "EngineDefines.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(PCGExSchedulingConstraintSource)

#pragma region UPCGExSchedulingConstraintDirectionAlignment

TOptional<double> UPCGExSchedulingConstraintDirectionAlignment::CalcPriority(const IPCGGenSourceBase* InGenSource, const FBox& InBounds, const bool bUse2DGrid) const
{
	const TOptional<FVector> Position = PCGExSchedulingShapes::GetSourcePosition(InGenSource);
	const TOptional<FVector> Direction = InGenSource ? InGenSource->GetDirection() : TOptional<FVector>();

	if (!Position.IsSet() || !Direction.IsSet())
	{
		return TOptional<double>();
	}

	const FVector SourcePosition = PCGExSchedulingShapes::SanitizedSourcePosition(Position.GetValue(), InBounds, bUse2DGrid);
	const FVector ToCell = InBounds.GetClosestPointTo(SourcePosition) - SourcePosition;

	if (ToCell.IsNearlyZero())
	{
		// Source inside the cell: maximum priority (mirrors the stock policy) -- flipped
		// when inverted, so 'favor cells behind' doesn't crown the source's own cell.
		return bInvert ? 0.0 : 1.0;
	}

	// Dot product remapped to [0, 1] -- the stock policy's Lerp(0.5, 1.0, Dot).
	double Alignment = (FVector::DotProduct(ToCell.GetSafeNormal(), Direction.GetValue()) + 1.0) * 0.5;
	if (bInvert)
	{
		Alignment = 1.0 - Alignment;
	}

	return Alignment;
}

void UPCGExSchedulingConstraintDirectionAlignment::DebugDraw(const UWorld* InWorld, const IPCGGenSourceBase* InGenSource, const bool bUse2DGrid, const bool bDrawCleanup) const
{
#if UE_ENABLE_DEBUG_DRAWING
	const TOptional<FVector> Position = PCGExSchedulingShapes::GetSourcePosition(InGenSource);
	const TOptional<FVector> Direction = InGenSource ? InGenSource->GetDirection() : TOptional<FVector>();

	if (!Position.IsSet() || !Direction.IsSet())
	{
		return;
	}

	// Inverted favors cells behind the source -- point the arrow that way.
	constexpr double ArrowLength = 1000.0;
	const FVector Favored = bInvert ? -Direction.GetValue() : Direction.GetValue();
	DrawDebugDirectionalArrow(InWorld, Position.GetValue(), Position.GetValue() + Favored * ArrowLength, 200.0f, PCGExSchedulingDebug::GateColor(bInvert), /*bPersistentLines=*/false, /*LifeTime=*/0.0f, /*DepthPriority=*/0, /*Thickness=*/2.0f);
#endif
}

#pragma endregion

#pragma region UPCGExSchedulingConstraintViewFrustum

bool UPCGExSchedulingConstraintViewFrustum::EvaluateGate(const IPCGGenSourceBase* InGenSource, const FBox& InBounds, const bool bUse2DGrid, const bool bExpanded) const
{
	const TOptional<FConvexVolume> ViewFrustum = InGenSource ? InGenSource->GetViewFrustum(bUse2DGrid) : TOptional<FConvexVolume>();
	if (!ViewFrustum.IsSet())
	{
		// Frustum-less sources are unaffected by this constraint, inverted or not.
		return true;
	}

	// Larger bounds modifier = more inclusive test. The cleanup (expanded) variant must be
	// MORE inclusive than generation for a regular gate, and LESS inclusive when inverted,
	// so the keep-alive region always supersets the generate region.
	const float EffectiveCleanupModifier = FMath::Max(GenerateBoundsModifier + 0.1f, CleanupBoundsModifier);
	float Modifier = GenerateBoundsModifier;

	if (bExpanded)
	{
		Modifier = bInvert
			           ? FMath::Max(0.05f, (GenerateBoundsModifier * GenerateBoundsModifier) / EffectiveCleanupModifier)
			           : EffectiveCleanupModifier;
	}

	FVector Center, Extents;
	InBounds.GetCenterAndExtents(Center, Extents);

	const bool bInside = ViewFrustum.GetValue().IntersectBox(Center, Extents * Modifier);

	return bInside != bInvert;
}

void UPCGExSchedulingConstraintViewFrustum::DebugDraw(const UWorld* InWorld, const IPCGGenSourceBase* InGenSource, const bool bUse2DGrid, const bool bDrawCleanup) const
{
#if UE_ENABLE_DEBUG_DRAWING
	const TOptional<FConvexVolume> ViewFrustum = InGenSource ? InGenSource->GetViewFrustum(bUse2DGrid) : TOptional<FConvexVolume>();
	if (!ViewFrustum.IsSet())
	{
		return;
	}

	// Engine sources build their frustum with GetViewFrustumBounds(near + far): plane order is
	// near, left, right, top, bottom, far. Anything else (a failed near plane) is not drawable.
	const FConvexVolume::FPlaneArray& Planes = ViewFrustum.GetValue().Planes;
	if (Planes.Num() != 6)
	{
		return;
	}

	// Corners[Depth][Side][Vertical]: Depth 0 = near / 1 = far, Side 0 = left / 1 = right, Vertical 0 = top / 1 = bottom.
	FVector Corners[2][2][2];
	const int32 DepthPlanes[2] = {0, 5};
	const int32 SidePlanes[2] = {1, 2};
	const int32 VerticalPlanes[2] = {3, 4};

	for (int32 Depth = 0; Depth < 2; ++Depth)
	{
		for (int32 Side = 0; Side < 2; ++Side)
		{
			for (int32 Vertical = 0; Vertical < 2; ++Vertical)
			{
				if (!FMath::IntersectPlanes3(Corners[Depth][Side][Vertical], Planes[DepthPlanes[Depth]], Planes[SidePlanes[Side]], Planes[VerticalPlanes[Vertical]]))
				{
					return;
				}
			}
		}
	}

	const FColor Color = PCGExSchedulingDebug::GateColor(bInvert);
	auto DrawEdge = [&](const FVector& InFrom, const FVector& InTo)
	{
		DrawDebugLine(InWorld, InFrom, InTo, Color, /*bPersistentLines=*/false, /*LifeTime=*/0.0f, /*DepthPriority=*/0, /*Thickness=*/0.0f);
	};

	for (int32 Depth = 0; Depth < 2; ++Depth)
	{
		// Near and far rectangles.
		DrawEdge(Corners[Depth][0][0], Corners[Depth][1][0]);
		DrawEdge(Corners[Depth][1][0], Corners[Depth][1][1]);
		DrawEdge(Corners[Depth][1][1], Corners[Depth][0][1]);
		DrawEdge(Corners[Depth][0][1], Corners[Depth][0][0]);
	}

	for (int32 Side = 0; Side < 2; ++Side)
	{
		for (int32 Vertical = 0; Vertical < 2; ++Vertical)
		{
			// Near-to-far edges.
			DrawEdge(Corners[0][Side][Vertical], Corners[1][Side][Vertical]);
		}
	}
#endif
}

#pragma endregion
