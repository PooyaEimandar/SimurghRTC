using UnrealBuildTool;
public class ContractHost : ModuleRules
{
    public ContractHost(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        PrivateDependencyModuleNames.AddRange(new string[] { "Core", "CoreUObject", "Engine", "SimurghRTCCore" });
    }
}
