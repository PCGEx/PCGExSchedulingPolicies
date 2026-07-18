// Copyright 2026 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

using UnrealBuildTool;

public class PCGExSchedulingPolicies : ModuleRules
{
	public PCGExSchedulingPolicies(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		IWYUSupport = IWYUSupport.Full;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"PCG",
			"DeveloperSettings", // UPCGExSchedulingSettings (UDeveloperSettings, public header)
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
		});

		if (Target.bBuildEditor)
		{
			// GEditor->GetAllViewportClients(): liveness validation of the runtime-gen editor
			// camera's raw FEditorViewportClient* (see PCGExSchedulingSubsystem.cpp).
			PrivateDependencyModuleNames.Add("UnrealEd");
		}
	}
}
