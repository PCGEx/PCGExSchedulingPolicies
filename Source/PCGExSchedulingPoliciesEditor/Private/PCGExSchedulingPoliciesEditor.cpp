// Copyright 2026 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#include "PCGExSchedulingPoliciesEditor.h"

#include "Modules/ModuleManager.h"

#define LOCTEXT_NAMESPACE "FPCGExSchedulingPoliciesEditorModule"

void FPCGExSchedulingPoliciesEditorModule::StartupModule()
{
}

void FPCGExSchedulingPoliciesEditorModule::ShutdownModule()
{
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FPCGExSchedulingPoliciesEditorModule, PCGExSchedulingPoliciesEditor)
