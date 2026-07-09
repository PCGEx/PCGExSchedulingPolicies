// Copyright 2026 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

using UnrealBuildTool;

public class PCGExSchedulingPoliciesEditor : ModuleRules
{
	public PCGExSchedulingPoliciesEditor(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		IWYUSupport = IWYUSupport.Full;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"PCGExSchedulingPolicies",
		});

		// Common editor foundations for detail customizations and Slate widgets.
		// Add DeveloperSettings / EditorWidgets / Projects / InputCore as features land.
		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"Slate",
			"SlateCore",
			"UnrealEd",
			"PropertyEditor",
		});
	}
}
