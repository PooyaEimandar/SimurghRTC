using UnrealBuildTool;

public class SimurghRTCMedia : SimurghRTCModuleRules
{
    public SimurghRTCMedia(ReadOnlyTargetRules Target) : base(Target)
    {
        PublicDependencyModuleNames.AddRange(new string[] { "Core", "CoreUObject", "Engine", "SimurghRTCCore" });
        PrivateDependencyModuleNames.AddRange(new string[] { "SimurghRTCNative", "RHI", "RenderCore", "AudioExtensions", "Json" });
    }
}
