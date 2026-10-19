using UnrealBuildTool;
public class ContractHostEditor : ModuleRules
{
    public ContractHostEditor(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        PrivateDependencyModuleNames.AddRange(new[] { "Core", "CoreUObject", "Engine", "UnrealEd", "BlueprintGraph",
            "KismetCompiler", "ContractHost", "SimurghRTCCore" });
    }
}
