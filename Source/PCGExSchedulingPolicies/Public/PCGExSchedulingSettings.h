// Copyright 2026 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"

#include "PCGExSchedulingCommon.h"

#include "PCGExSchedulingSettings.generated.h"

/** A named scheduling channel, plus the actor/component tags that grant it. */
USTRUCT()
struct PCGEXSCHEDULINGPOLICIES_API FPCGExSchedulingChannel
{
	GENERATED_BODY()

	/** Unique channel name. */
	UPROPERTY(EditAnywhere, Category = Settings)
	FName Name = NAME_None;

	/**
	 * Actor or component tags that grant this channel to a generation source
	 * (vanilla PCG Generation Source components, player controllers and their pawns).
	 * Tag-to-channel mapping lives here only — single source of truth.
	 */
	UPROPERTY(EditAnywhere, Category = Settings)
	TArray<FName> Tags;
};

/**
 * Project-wide settings for PCGEx scheduling policies: channel definitions and
 * channel grants for built-in generation sources.
 *
 * Threading: lookup tables are rebuilt on the game thread (load/edit) and are only
 * read from game-thread resolution paths (see UPCGExSchedulingSubsystem).
 */
UCLASS(Config = Game, DefaultConfig, meta = (DisplayName = "PCGEx | Scheduling Policies"))
class PCGEXSCHEDULINGPOLICIES_API UPCGExSchedulingSettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	//~ Begin UDeveloperSettings interface
	virtual FName GetContainerName() const override { return FName("Project"); }
	virtual FName GetCategoryName() const override { return FName("Plugins"); }
	virtual FName GetSectionName() const override { return FName("PCGEx | Scheduling Policies"); }
	//~ End UDeveloperSettings interface

	virtual void PostInitProperties() override;

#if WITH_EDITOR
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;

	/** Broadcast whenever channel definitions change (editor) — pickers and caches listen to this. */
	static FSimpleMulticastDelegate OnChannelsChanged;
#endif

	/** Channel definitions (max 64). Selectors reference channels by name, so reordering or renaming entries is safe. */
	UPROPERTY(EditAnywhere, Config, Category = "Channels", meta = (TitleProperty = "Name"))
	TArray<FPCGExSchedulingChannel> Channels;

	/**
	 * Channels granted to automatic player generation sources.
	 * Player controller and possessed pawn actor tags additionally resolve through channel tags.
	 */
	UPROPERTY(EditAnywhere, Config, Category = "Channels")
	FPCGExChannelSelector PlayerChannels;

	/** Seconds between periodic re-resolutions of cached source channel masks (picks up runtime tag changes, pawn possession, etc.). */
	UPROPERTY(EditAnywhere, Config, Category = "Channels", AdvancedDisplay, meta = (ClampMin = 0.1))
	float SourceMaskRefreshInterval = 2.0f;

	/** Resolve channel names to a runtime mask. Unknown names are ignored. Game thread. */
	PCGExScheduling::FChannelMask ResolveChannelNames(const TArray<FName>& InNames) const;

	/** Resolve actor/component tags to a runtime mask via the channel tag lists. Game thread. */
	PCGExScheduling::FChannelMask ResolveTags(const TArray<FName>& InTags) const;

	/** Monotonic revision, bumped whenever channel lookups are rebuilt. Never 0 after initialization. */
	uint32 GetRevision() const { return Revision; }

private:
	void RebuildLookups();

	/** Channel name → single-bit mask. */
	TMap<FName, PCGExScheduling::FChannelMask> NameToMask;

	/** Tag → mask of all channels granting it. */
	TMap<FName, PCGExScheduling::FChannelMask> TagToMask;

	uint32 Revision = 0;
};
