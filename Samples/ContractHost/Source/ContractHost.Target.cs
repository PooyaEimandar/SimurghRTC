using UnrealBuildTool;
using System.IO;
public class ContractHostTarget : TargetRules
{
    public ContractHostTarget(TargetInfo Target) : base(Target)
    {
        Type = TargetType.Game;
        DefaultBuildSettings = BuildSettingsVersion.Latest;
        IncludeOrderVersion = EngineIncludeOrderVersion.Latest;
        ExtraModuleNames.Add("ContractHost");
        if (Platform == UnrealTargetPlatform.IOS && Architecture == UnrealArch.IOSSimulator)
        {
            // UE 5.8.3's IoStore HTTP client otherwise selects a device-only nghttp2 archive.
            // Load the matching simulator implementation first, without changing engine files.
            // UAT compiles target scripts separately from module rules. The module validates this archive.
            string Support = Path.Combine(ProjectFile!.Directory.FullName, "Build", "IOSSimulator", "libnghttp2.a");
            AdditionalLinkerArguments = "-Wl,-force_load,\"" + Support + "\"";
        }
    }
}
