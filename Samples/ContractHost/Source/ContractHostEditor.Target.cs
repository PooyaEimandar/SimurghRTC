using UnrealBuildTool;
public class ContractHostEditorTarget : TargetRules
{
    public ContractHostEditorTarget(TargetInfo Target) : base(Target)
    {
        Type = TargetType.Editor;
        DefaultBuildSettings = BuildSettingsVersion.Latest;
        IncludeOrderVersion = EngineIncludeOrderVersion.Latest;
        ExtraModuleNames.Add("ContractHost");
        // Opt in when building a source Editor for the packaged mobile probes.
        if (Target.Arguments?.HasOption("-SimurghRTCAndroidCook") == true)
        {
            ExtraModuleNames.AddRange(new[] { "AndroidTargetPlatform", "AndroidTargetPlatformSettings",
                "AndroidTargetPlatformControls", "TextureFormatASTC", "TextureFormatETC2" });
        }
        if (Target.Arguments?.HasOption("-SimurghRTCIOSCook") == true)
        {
            ExtraModuleNames.AddRange(new[] { "IOSTargetPlatform", "IOSTargetPlatformSettings",
                "IOSTargetPlatformControls", "MetalShaderFormat", "TextureFormatASTC" });
        }
    }
}
