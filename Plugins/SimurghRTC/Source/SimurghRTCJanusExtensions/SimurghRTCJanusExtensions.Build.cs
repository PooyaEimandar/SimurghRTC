using UnrealBuildTool;

public class SimurghRTCJanusExtensions : SimurghRTCModuleRules
{
    public SimurghRTCJanusExtensions(ReadOnlyTargetRules Target) : base(Target)
    {
        PublicDependencyModuleNames.AddRange(new string[] { "Core", "CoreUObject", "Engine", "SimurghRTCCore", "SimurghRTCJanus" });
    }
}
