using UnrealBuildTool;

public class SimurghRTCNative : SimurghRTCModuleRules
{
    public SimurghRTCNative(ReadOnlyTargetRules Target) : base(Target)
    {
        PublicDependencyModuleNames.AddRange(new string[] { "Core", "SimurghRTCCore" });
        bool UseEngineWebRTC = Target.Platform == UnrealTargetPlatform.Mac
            || Target.Platform == UnrealTargetPlatform.Win64
            || (Target.Platform == UnrealTargetPlatform.Linux && Target.Architecture == UnrealArch.X64);
        PrivateDefinitions.Add("SIMURGHRTC_WITH_ENGINE_WEBRTC=" + (UseEngineWebRTC ? "1" : "0"));
        if (UseEngineWebRTC)
        {
            if (Target.Version.MajorVersion != 5 || Target.Version.MinorVersion != 8 || Target.Version.PatchVersion != 3)
                throw new BuildException("The SimurghRTC native adapter currently targets Unreal Engine 5.8.3. Review the adapter before using another version.");
            // Explicitly include archive dependencies: precompiled engine external modules may not propagate private links.
            AddEngineThirdPartyPrivateStaticDependencies(Target, "WebRTC", "OpenSSL", "libOpus", "LibVpx");
            if (Target.Platform == UnrealTargetPlatform.Mac)
                PublicFrameworks.AddRange(new string[] { "AudioToolbox", "CoreAudio", "CoreVideo", "VideoToolbox" });
        }
        // Android/iOS intentionally do not fall back to engine libraries. Separate artifacts follow in SR-006/007.
    }
}
