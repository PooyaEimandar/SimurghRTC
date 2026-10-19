using System;
using System.IO;
using EpicGames.Core;
using UnrealBuildTool;

// All modules consume the same generated profile. No environment-variable feature switches.
public abstract class SimurghRTCModuleRules : ModuleRules
{
    protected SimurghRTCModuleRules(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        string ProfilePath = Path.Combine(PluginDirectory, "SimurghRTC.profile.json");
        ExternalDependencies.Add(ProfilePath);
        JsonObject Profile = JsonObject.Read(new FileReference(ProfilePath));
        if (Profile.GetIntegerField("schema") != 1)
            throw new BuildException("Unsupported SimurghRTC profile schema. Regenerate the profile.");
        bool IsServer = Target.Type == TargetType.Server;
        bool Direct = Profile.GetBoolField("with_direct");
        bool Janus = Profile.GetBoolField("with_janus") && !IsServer;
        bool JanusExtensions = Profile.GetBoolField("with_janus_extensions") && !IsServer;
        bool Media = Profile.GetBoolField("with_media") && !IsServer;
        if (IsServer && !Profile.GetBoolField("with_server"))
            throw new BuildException("This SimurghRTC profile excludes the Unreal Server endpoint.");
        PublicDefinitions.Add("SIMURGHRTC_WITH_DIRECT=" + (Direct ? "1" : "0"));
        PublicDefinitions.Add("SIMURGHRTC_WITH_JANUS=" + (Janus ? "1" : "0"));
        PublicDefinitions.Add("SIMURGHRTC_WITH_JANUS_EXTENSIONS=" + (JanusExtensions ? "1" : "0"));
        PublicDefinitions.Add("SIMURGHRTC_WITH_MEDIA=" + (Media ? "1" : "0"));
        // The M0 contract shell never claims a linked/usable native transport.
        PublicDefinitions.Add("SIMURGHRTC_WITH_NATIVE_TRANSPORT=0");
    }
}

public class SimurghRTCCore : SimurghRTCModuleRules
{
    public SimurghRTCCore(ReadOnlyTargetRules Target) : base(Target)
    {
        PublicDependencyModuleNames.AddRange(new string[] { "Core", "CoreUObject", "Engine" });
    }
}
