using UnrealBuildTool;

public class SimurghRTCDirect : SimurghRTCModuleRules
{
    public SimurghRTCDirect(ReadOnlyTargetRules Target) : base(Target)
    {
        PublicDependencyModuleNames.AddRange(new string[] { "Core", "SimurghRTCCore", "SimurghRTCNative" });
    }
}
