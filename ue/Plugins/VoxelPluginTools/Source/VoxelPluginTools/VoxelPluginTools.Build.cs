using UnrealBuildTool;

public class VoxelPluginTools : ModuleRules
{
	public VoxelPluginTools(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;
		// Each handler file keeps its helpers in an anonymous namespace.
		bUseUnity = false;

		PublicDependencyModuleNames.AddRange(new string[] { "Core", "CoreUObject", "Engine", "Json" });

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"UnrealEd",
			"AssetRegistry",
			"AssetTools",
			"PCG",
			"VoxelPCG",
			"UE_MCP_Bridge",
			"VoxelCore",
			"VoxelGraph",
			"Voxel",
		});
	}
}
