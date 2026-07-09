// Copyright 2026 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#include "PCGExChannelSelectorCustomization.h"

#include "PCGExSchedulingCommon.h"
#include "PCGExSchedulingSettings.h"

#include "DetailWidgetRow.h"
#include "ISettingsModule.h"
#include "PropertyHandle.h"
#include "ScopedTransaction.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "Modules/ModuleManager.h"
#include "Widgets/Input/SComboButton.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "PCGExChannelSelectorCustomization"

TSharedRef<IPropertyTypeCustomization> FPCGExChannelSelectorCustomization::MakeInstance()
{
	return MakeShared<FPCGExChannelSelectorCustomization>();
}

void FPCGExChannelSelectorCustomization::CustomizeHeader(const TSharedRef<IPropertyHandle> InStructHandle, FDetailWidgetRow& HeaderRow, IPropertyTypeCustomizationUtils& CustomizationUtils)
{
	StructHandle = InStructHandle;

	HeaderRow
		.NameContent()
		[
			InStructHandle->CreatePropertyNameWidget()
		]
		.ValueContent()
		.MinDesiredWidth(250.0f)
		[
			SNew(SComboButton)
			.OnGetMenuContent(this, &FPCGExChannelSelectorCustomization::BuildChannelsMenu)
			.ContentPadding(FMargin(4.0f, 2.0f))
			.ButtonContent()
			[
				SNew(STextBlock)
				.Font(CustomizationUtils.GetRegularFont())
				.Text(this, &FPCGExChannelSelectorCustomization::GetSummaryText)
			]
		];
}

void FPCGExChannelSelectorCustomization::CustomizeChildren(const TSharedRef<IPropertyHandle> InStructHandle, IDetailChildrenBuilder& ChildBuilder, IPropertyTypeCustomizationUtils& CustomizationUtils)
{
	// Header-only customization.
}

TArray<FName> FPCGExChannelSelectorCustomization::GetSelection() const
{
	TArray<FName> Selection;

	if (StructHandle.IsValid())
	{
		// Multi-object edits display/edit through the first object.
		StructHandle->EnumerateConstRawData(
			[&Selection](const void* RawData, const int32, const int32)
			{
				if (RawData)
				{
					Selection = static_cast<const FPCGExChannelSelector*>(RawData)->Channels;
					return false;
				}
				return true;
			});
	}

	return Selection;
}

void FPCGExChannelSelectorCustomization::SetSelection(const TArray<FName>& InSelection) const
{
	if (!StructHandle.IsValid())
	{
		return;
	}

	FScopedTransaction Transaction(LOCTEXT("SetChannels", "Set Channels"));

	StructHandle->NotifyPreChange();

	StructHandle->EnumerateRawData(
		[&InSelection](void* RawData, const int32, const int32)
		{
			if (RawData)
			{
				static_cast<FPCGExChannelSelector*>(RawData)->Channels = InSelection;
			}
			return true;
		});

	StructHandle->NotifyPostChange(EPropertyChangeType::ValueSet);
	StructHandle->NotifyFinishedChangingProperties();
}

void FPCGExChannelSelectorCustomization::ToggleChannel(const FName InChannel) const
{
	TArray<FName> Selection = GetSelection();
	if (Selection.Remove(InChannel) == 0)
	{
		Selection.Add(InChannel);
	}

	SetSelection(Selection);
}

bool FPCGExChannelSelectorCustomization::IsChannelSelected(const FName InChannel) const
{
	return GetSelection().Contains(InChannel);
}

FText FPCGExChannelSelectorCustomization::GetSummaryText() const
{
	const TArray<FName> Selection = GetSelection();

	if (Selection.IsEmpty())
	{
		return LOCTEXT("NoChannels", "None");
	}

	FString Summary;
	for (const FName& Channel : Selection)
	{
		if (!Summary.IsEmpty()) { Summary += TEXT(", "); }
		Summary += Channel.ToString();
	}

	return FText::FromString(Summary);
}

TSharedRef<SWidget> FPCGExChannelSelectorCustomization::BuildChannelsMenu() const
{
	// Keep the menu open across toggles — channel selection is a multi-pick.
	FMenuBuilder MenuBuilder(/*bInShouldCloseWindowAfterMenuSelection=*/false, /*InCommandList=*/nullptr);

	const UPCGExSchedulingSettings* Settings = GetDefault<UPCGExSchedulingSettings>();
	TSet<FName> KnownChannels;

	MenuBuilder.BeginSection(NAME_None, LOCTEXT("ChannelsSection", "Channels"));

	for (const FPCGExSchedulingChannel& Channel : Settings->Channels)
	{
		if (Channel.Name.IsNone() || KnownChannels.Contains(Channel.Name))
		{
			continue;
		}

		KnownChannels.Add(Channel.Name);

		const FName ChannelName = Channel.Name;
		MenuBuilder.AddMenuEntry(
			FText::FromName(ChannelName),
			FText::Format(LOCTEXT("ChannelTooltipFmt", "Toggle channel '{0}'."), FText::FromName(ChannelName)),
			FSlateIcon(),
			FUIAction(
				FExecuteAction::CreateSP(this, &FPCGExChannelSelectorCustomization::ToggleChannel, ChannelName),
				FCanExecuteAction(),
				FIsActionChecked::CreateSP(this, &FPCGExChannelSelectorCustomization::IsChannelSelected, ChannelName)),
			NAME_None,
			EUserInterfaceActionType::Check);
	}

	if (KnownChannels.IsEmpty())
	{
		MenuBuilder.AddMenuEntry(
			LOCTEXT("NoChannelsDefined", "No channels defined"),
			LOCTEXT("NoChannelsDefinedTooltip", "Define scheduling channels in Project Settings → Plugins → PCGEx | Scheduling Policies."),
			FSlateIcon(),
			FUIAction(FExecuteAction(), FCanExecuteAction::CreateLambda([] { return false; })));
	}

	MenuBuilder.EndSection();

	// Selected names that no longer exist in the settings — flag and allow removal.
	TArray<FName> UnknownChannels;
	for (const FName& Channel : GetSelection())
	{
		if (!KnownChannels.Contains(Channel))
		{
			UnknownChannels.Add(Channel);
		}
	}

	if (!UnknownChannels.IsEmpty())
	{
		MenuBuilder.BeginSection(NAME_None, LOCTEXT("UnknownChannelsSection", "Unknown Channels"));

		for (const FName& Channel : UnknownChannels)
		{
			MenuBuilder.AddMenuEntry(
				FText::Format(LOCTEXT("UnknownChannelFmt", "{0} (unknown)"), FText::FromName(Channel)),
				LOCTEXT("UnknownChannelTooltip", "This channel is not defined in the settings. Toggle to remove it from the selection."),
				FSlateIcon(),
				FUIAction(
					FExecuteAction::CreateSP(this, &FPCGExChannelSelectorCustomization::ToggleChannel, Channel),
					FCanExecuteAction(),
					FIsActionChecked::CreateSP(this, &FPCGExChannelSelectorCustomization::IsChannelSelected, Channel)),
				NAME_None,
				EUserInterfaceActionType::Check);
		}

		MenuBuilder.EndSection();
	}

	MenuBuilder.BeginSection(NAME_None, FText::GetEmpty());
	MenuBuilder.AddMenuEntry(
		LOCTEXT("ManageChannels", "Manage Channels…"),
		LOCTEXT("ManageChannelsTooltip", "Open the PCGEx | Scheduling Policies project settings."),
		FSlateIcon(),
		FUIAction(FExecuteAction::CreateStatic(&FPCGExChannelSelectorCustomization::OpenChannelSettings)));
	MenuBuilder.EndSection();

	return MenuBuilder.MakeWidget();
}

void FPCGExChannelSelectorCustomization::OpenChannelSettings()
{
	if (ISettingsModule* SettingsModule = FModuleManager::GetModulePtr<ISettingsModule>("Settings"))
	{
		const UPCGExSchedulingSettings* Settings = GetDefault<UPCGExSchedulingSettings>();
		SettingsModule->ShowViewer(Settings->GetContainerName(), Settings->GetCategoryName(), Settings->GetSectionName());
	}
}

#undef LOCTEXT_NAMESPACE
