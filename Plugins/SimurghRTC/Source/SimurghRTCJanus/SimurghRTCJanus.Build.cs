using UnrealBuildTool;

public class SimurghRTCJanus : SimurghRTCModuleRules
{
    public SimurghRTCJanus(ReadOnlyTargetRules Target) : base(Target)
    {
        PublicDependencyModuleNames.AddRange(new string[] { "Core", "SimurghRTCCore", "SimurghRTCNative" });
    }
}
