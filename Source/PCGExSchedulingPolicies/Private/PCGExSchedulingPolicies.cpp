// Copyright 2026 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#include "PCGExSchedulingPolicies.h"

#include "PCGExSchedulingCommon.h"

#include "Modules/ModuleManager.h"

DEFINE_LOG_CATEGORY(LogPCGExScheduling)

#define LOCTEXT_NAMESPACE "FPCGExSchedulingPoliciesModule"

void FPCGExSchedulingPoliciesModule::StartupModule()
{
}

void FPCGExSchedulingPoliciesModule::ShutdownModule()
{
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FPCGExSchedulingPoliciesModule, PCGExSchedulingPolicies)
