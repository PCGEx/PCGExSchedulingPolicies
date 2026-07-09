// Copyright 2026 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#include "PCGExSchedulingSettings.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(PCGExSchedulingSettings)

#if WITH_EDITOR
FSimpleMulticastDelegate UPCGExSchedulingSettings::OnChannelsChanged;
#endif

void UPCGExSchedulingSettings::PostInitProperties()
{
	Super::PostInitProperties();
	RebuildLookups();
}

#if WITH_EDITOR
void UPCGExSchedulingSettings::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);

	RebuildLookups();
	OnChannelsChanged.Broadcast();
}
#endif

PCGExScheduling::FChannelMask UPCGExSchedulingSettings::ResolveChannelNames(const TArray<FName>& InNames) const
{
	PCGExScheduling::FChannelMask Mask = 0;
	for (const FName& Name : InNames)
	{
		if (const PCGExScheduling::FChannelMask* Found = NameToMask.Find(Name))
		{
			Mask |= *Found;
		}
	}
	return Mask;
}

PCGExScheduling::FChannelMask UPCGExSchedulingSettings::ResolveTags(const TArray<FName>& InTags) const
{
	PCGExScheduling::FChannelMask Mask = 0;
	for (const FName& Tag : InTags)
	{
		if (const PCGExScheduling::FChannelMask* Found = TagToMask.Find(Tag))
		{
			Mask |= *Found;
		}
	}
	return Mask;
}

void UPCGExSchedulingSettings::GetChannelTable(TArray<TPair<FName, PCGExScheduling::FChannelMask>>& OutChannelTable) const
{
	OutChannelTable.Reset();
	OutChannelTable.Reserve(NameToMask.Num());

	// Preserve the settings-declared order (NameToMask iteration order is unstable).
	PCGExScheduling::FChannelMask EmittedBits = 0;
	for (const FPCGExSchedulingChannel& Channel : Channels)
	{
		if (const PCGExScheduling::FChannelMask* Found = NameToMask.Find(Channel.Name))
		{
			// Skip duplicate-named entries (only the first owns the bit).
			if ((EmittedBits & *Found) == 0)
			{
				EmittedBits |= *Found;
				OutChannelTable.Emplace(Channel.Name, *Found);
			}
		}
	}
}

void UPCGExSchedulingSettings::RebuildLookups()
{
	NameToMask.Reset();
	TagToMask.Reset();

	int32 Bit = 0;
	TArray<FString> IgnoredChannels;

	for (const FPCGExSchedulingChannel& Channel : Channels)
	{
		if (Channel.Name.IsNone())
		{
			continue;
		}

		if (NameToMask.Contains(Channel.Name))
		{
			UE_LOG(LogPCGExScheduling, Warning, TEXT("Duplicate scheduling channel '%s' ignored -- only the first entry owns the channel."), *Channel.Name.ToString());
			continue;
		}

		if (Bit >= PCGExScheduling::MaxChannels)
		{
			IgnoredChannels.Add(Channel.Name.ToString());
			continue;
		}

		const PCGExScheduling::FChannelMask Mask = PCGExScheduling::FChannelMask(1) << Bit++;
		NameToMask.Add(Channel.Name, Mask);

		for (const FName& Tag : Channel.Tags)
		{
			if (!Tag.IsNone())
			{
				TagToMask.FindOrAdd(Tag) |= Mask;
			}
		}
	}

	if (!IgnoredChannels.IsEmpty())
	{
		UE_LOG(LogPCGExScheduling, Warning, TEXT("%d scheduling channel(s) ignored -- only %d channels are supported: %s"),
			IgnoredChannels.Num(), PCGExScheduling::MaxChannels, *FString::Join(IgnoredChannels, TEXT(", ")));
	}

	++Revision;
	if (Revision == 0) { ++Revision; }
}
