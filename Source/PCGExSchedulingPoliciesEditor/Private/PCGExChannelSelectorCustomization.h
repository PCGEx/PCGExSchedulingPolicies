// Copyright 2026 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#pragma once

#include "CoreMinimal.h"
#include "IPropertyTypeCustomization.h"

class IPropertyHandle;

/**
 * Picker for FPCGExChannelSelector: a checkbox menu of the channels defined in
 * Project Settings → Plugins → PCGEx | Scheduling Policies, with stale entries flagged.
 */
class FPCGExChannelSelectorCustomization : public IPropertyTypeCustomization
{
public:
	static TSharedRef<IPropertyTypeCustomization> MakeInstance();

	//~ Begin IPropertyTypeCustomization interface
	virtual void CustomizeHeader(TSharedRef<IPropertyHandle> InStructHandle, FDetailWidgetRow& HeaderRow, IPropertyTypeCustomizationUtils& CustomizationUtils) override;
	virtual void CustomizeChildren(TSharedRef<IPropertyHandle> InStructHandle, IDetailChildrenBuilder& ChildBuilder, IPropertyTypeCustomizationUtils& CustomizationUtils) override;
	//~ End IPropertyTypeCustomization interface

private:
	TArray<FName> GetSelection() const;
	void ToggleChannel(FName InChannel) const;
	bool IsChannelSelected(FName InChannel) const;
	FText GetSummaryText() const;
	TSharedRef<SWidget> BuildChannelsMenu() const;

	static void OpenChannelSettings();

	TSharedPtr<IPropertyHandle> StructHandle;
};
