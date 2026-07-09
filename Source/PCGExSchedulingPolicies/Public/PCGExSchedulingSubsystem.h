// Copyright 2026 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#pragma once

#include "CoreMinimal.h"
#include "HAL/CriticalSection.h"
#include "Misc/ScopeRWLock.h"
#include "Subsystems/WorldSubsystem.h"
#include "UObject/ObjectKey.h"

#include "PCGExSchedulingCommon.h"

#include "PCGExSchedulingSubsystem.generated.h"

class IPCGGenSourceBase;

/**
 * World subsystem backing PCGEx scheduling policies: resolves and caches
 * per-generation-source channel masks.
 *
 * Threading contract:
 * - ResolveSourceMask() is game-thread only (touches actors and settings).
 * - GetCachedSourceMask() is safe from any thread (lock-protected read of the last resolved value).
 *   The runtime-gen scheduler evaluates ShouldGenerate on the game thread and ShouldCull on worker
 *   threads; cleanup only ever visits cells a prior game-thread scan already resolved, so worker
 *   reads find a (possibly one-tick stale) entry.
 */
UCLASS()
class PCGEXSCHEDULINGPOLICIES_API UPCGExSchedulingSubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	/** Returns the subsystem for the given world, if it exists and is initialized. */
	static UPCGExSchedulingSubsystem* GetInstance(const UWorld* InWorld);

	//~ Begin UTickableWorldSubsystem interface
	virtual void Tick(float DeltaSeconds) override;
	virtual ETickableTickType GetTickableTickType() const override { return IsTemplate() ? ETickableTickType::Never : ETickableTickType::Conditional; }
	virtual bool IsTickable() const override;
	virtual bool IsTickableInEditor() const override { return true; }
	virtual TStatId GetStatId() const override;
	//~ End UTickableWorldSubsystem interface

	/** Resolves (and caches) the channel mask of a generation source. Game thread only. */
	PCGExScheduling::FChannelMask ResolveSourceMask(const IPCGGenSourceBase* InGenSource);

	/** Last resolved channel mask of a generation source, if any. Safe from any thread; never resolves. */
	TOptional<PCGExScheduling::FChannelMask> GetCachedSourceMask(const IPCGGenSourceBase* InGenSource) const;

	/** Resolves on the game thread, falls back to the cached value on worker threads. */
	TOptional<PCGExScheduling::FChannelMask> GetOrResolveSourceMask(const IPCGGenSourceBase* InGenSource);

	/** Drops the cached entry for a source so the next resolution recomputes it (e.g. channels edited on a component). */
	void InvalidateSource(const UObject* InSourceObject);

protected:
	/** Uncached resolution ladder. Game thread only. */
	PCGExScheduling::FChannelMask ResolveMaskInternal(const IPCGGenSourceBase* InGenSource) const;

	struct FResolvedSource
	{
		PCGExScheduling::FChannelMask Mask = 0;
		uint32 Revision = 0;
	};

	mutable FRWLock SourceMasksLock;
	TMap<FObjectKey, FResolvedSource> SourceMasks;

	/** Last periodic maintenance timestamp (re-resolution + dead key cleanup). */
	double LastMaintenanceTime = 0.0;
};
