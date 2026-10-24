using UnrealBuildTool;

public class SimurghRTCServer : SimurghRTCModuleRules
{
    public SimurghRTCServer(ReadOnlyTargetRules Target) : base(Target)
    {
        PublicDependencyModuleNames.AddRange(new string[] { "Core", "SimurghRTCCore", "SimurghRTCNative", "SimurghRTCDirect" });
        PrivateDependencyModuleNames.AddRange(new string[] { "HTTPServer", "Json", "CoreUObject", "Engine" });
    }
}
