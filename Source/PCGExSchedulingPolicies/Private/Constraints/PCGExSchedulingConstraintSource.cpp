// Copyright 2026 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#include "Constraints/PCGExSchedulingConstraintSource.h"

#include "Constraints/PCGExSchedulingConstraintShapes.h"
#include "RuntimeGen/GenSources/PCGGenSourceBase.h"

#include "ConvexVolume.h"

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

#pragma endregion
