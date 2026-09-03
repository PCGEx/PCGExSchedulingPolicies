// Copyright 2026 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#pragma once

#include "CoreMinimal.h"

#include "PCGExSchedulingConstraint.generated.h"

class IPCGGenSourceBase;
class UWorld;

namespace PCGExSchedulingDebug
{
	/** Gate region: green, cyan when inverted. */
	FORCEINLINE FColor GateColor(const bool bInvert) { return bInvert ? FColor::Cyan : FColor::Green; }

	/** Cleanup (hysteresis) variant. */
	FORCEINLINE FColor CleanupColor() { return FColor::Orange; }

	/** Hollow interior. */
	FORCEINLINE FColor InteriorColor() { return FColor::Red; }
}

/**
 * A single composable scheduling constraint, stacked on a UPCGExSchedulingPolicy.
 * Extending the system means adding a constraint class -- never a new policy.
 *
 * Threading contract (verified against the 5.8 runtime-gen scheduler):
 * - EvaluateGate with bExpanded=false, CalcPriority and DebugDraw run on the game thread, outside any
 *   parallel loop. They may refresh cached state.
 * - EvaluateGate with bExpanded=true (cleanup) runs inside a ParallelFor that the game thread joins:
 *   game-thread and worker calls are CONCURRENT. It must be strictly read-only -- no member writes,
 *   whatever IsInGameThread() says. Cleanup reads whatever the last scan cached.
 */
UCLASS(Abstract, BlueprintType, Blueprintable, EditInlineNew, DefaultToInstanced, CollapseCategories, ClassGroup = (Procedural))
class PCGEXSCHEDULINGPOLICIES_API UPCGExSchedulingConstraint : public UObject
{
	GENERATED_BODY()

public:
	/** Disabled constraints are skipped entirely (no gate, no priority contribution). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = Constraint)
	bool bEnabled = true;

	/** Inverts the gate. Implementations also flip their cleanup hysteresis (expanded becomes shrunk) so inverted boundaries don't flicker. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = Constraint)
	bool bInvert = false;

	/** Weight of this constraint's priority contribution when blending against other constraints. 0 = no contribution. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = Constraint, meta = (ClampMin = 0))
	float PriorityWeight = 1.0f;

	/**
	 * Gate test for a grid cell against a generation source. Must honor bInvert.
	 * bExpanded=true evaluates the enlarged cleanup variant (hysteresis) used by ShouldCull --
	 * read-only and concurrent, see the class threading contract.
	 */
	virtual bool EvaluateGate(const IPCGGenSourceBase* InGenSource, const FBox& InBounds, bool bUse2DGrid, bool bExpanded) const { return true; }

	/** Priority contribution in [0,1]; unset = no contribution (gate-only constraints). Game thread. */
	virtual TOptional<double> CalcPriority(const IPCGGenSourceBase* InGenSource, const FBox& InBounds, bool bUse2DGrid) const { return TOptional<double>(); }

	/** True when this constraint's answers depend on the generation source's direction (drives rescan-on-rotation and frustum caching). */
	virtual bool CullsBasedOnDirection() const { return false; }

	/**
	 * Same-class, property-identical comparison (reflection walk) -- subclass properties are
	 * covered automatically. Override only for non-property state.
	 */
	virtual bool IsEquivalentTo(const UPCGExSchedulingConstraint* InOther) const;

	/** False for constraints whose regions don't depend on the generation source -- drawn once, with a null source. */
	virtual bool DebugDrawsPerSource() const { return true; }

	/**
	 * Draws this constraint's regions (pcgex.Scheduling.DebugDraw). InGenSource is null when
	 * DebugDrawsPerSource is false. Game thread; only called when debug drawing is compiled in.
	 */
	virtual void DebugDraw(const UWorld* InWorld, const IPCGGenSourceBase* InGenSource, bool bUse2DGrid, bool bDrawCleanup) const {}
};
