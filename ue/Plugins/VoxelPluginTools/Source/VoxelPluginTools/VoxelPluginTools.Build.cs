using UnrealBuildTool;

public class VoxelPluginTools : ModuleRules
{
	public VoxelPluginTools(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[] { "Core", "CoreUObject", "Engine", "Json" });

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"UnrealEd",
			"UE_MCP_Bridge",
			"VoxelCore",
			"VoxelGraph",
			"Voxel",
		});
	}
}
