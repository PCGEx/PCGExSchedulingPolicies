// Copyright 2026 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#include "Constraints/PCGExSchedulingConstraintShapes.h"

#include "RuntimeGen/GenSources/PCGGenSourceBase.h"

#include "Math/Box2D.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(PCGExSchedulingConstraintShapes)

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
	const bool bInside = InBounds.ComputeSquaredDistanceToPoint(SourcePosition) <= ScaledRadius * ScaledRadius;

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
