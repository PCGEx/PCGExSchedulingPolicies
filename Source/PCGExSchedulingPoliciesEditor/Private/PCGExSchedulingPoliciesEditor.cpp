// Copyright 2026 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#include "PCGExSchedulingPoliciesEditor.h"

#include "PropertyEditorModule.h"
#include "Modules/ModuleManager.h"

#include "PCGExChannelSelectorCustomization.h"
#include "PCGExSchedulingCommon.h"

#define LOCTEXT_NAMESPACE "FPCGExSchedulingPoliciesEditorModule"

void FPCGExSchedulingPoliciesEditorModule::StartupModule()
{
	FPropertyEditorModule& PropertyModule = FModuleManager::LoadModuleChecked<FPropertyEditorModule>("PropertyEditor");

	PropertyModule.RegisterCustomPropertyTypeLayout(
		FPCGExChannelSelector::StaticStruct()->GetFName(),
		FOnGetPropertyTypeCustomizationInstance::CreateStatic(&FPCGExChannelSelectorCustomization::MakeInstance));

	PropertyModule.NotifyCustomizationModuleChanged();
}

void FPCGExSchedulingPoliciesEditorModule::ShutdownModule()
{
	if (FModuleManager::Get().IsModuleLoaded("PropertyEditor"))
	{
		FPropertyEditorModule& PropertyModule = FModuleManager::GetModuleChecked<FPropertyEditorModule>("PropertyEditor");
		PropertyModule.UnregisterCustomPropertyTypeLayout(FPCGExChannelSelector::StaticStruct()->GetFName());
	}
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FPCGExSchedulingPoliciesEditorModule, PCGExSchedulingPoliciesEditor)
