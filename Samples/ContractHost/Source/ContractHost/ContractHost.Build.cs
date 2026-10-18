using UnrealBuildTool;
using System;
using System.IO;
using System.Security.Cryptography;
using System.Text.Json;
public class ContractHost : ModuleRules
{
    public ContractHost(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        PrivateDependencyModuleNames.AddRange(new string[] { "Core", "CoreUObject", "Engine", "Json", "SimurghRTCCore", "SimurghRTCNative" });
        if (Target.Platform == UnrealTargetPlatform.IOS && Target.Architecture == UnrealArch.IOSSimulator)
        {
            string Support = ContractHostSimulatorSupport.Library(Target.ProjectFile!.Directory.FullName);
            PublicAdditionalLibraries.Add(Support); // Track the archive as a link prerequisite as well.
            ExternalDependencies.Add(Path.Combine(Path.GetDirectoryName(Support)!, "support.json"));
        }
    }
}

public static class ContractHostSimulatorSupport
{
    public static string Library(string ProjectDirectory)
    {
        string Root = Path.Combine(ProjectDirectory, "Build", "IOSSimulator");
        string Archive = Path.Combine(Root, "libnghttp2.a");
        string Manifest = Path.Combine(Root, "support.json");
        if (!File.Exists(Archive) || !File.Exists(Manifest))
            throw new BuildException("Build tools/ios_simulator_support.py and stage with --simulator-support; see docs/MOBILE_PROBE.md.");
        using FileStream ManifestStream = File.OpenRead(Manifest);
        using JsonDocument Document = JsonDocument.ParseAsync(ManifestStream).GetAwaiter().GetResult();
        JsonElement Metadata = Document.RootElement;
        using FileStream Stream = File.OpenRead(Archive);
        string Hash = Convert.ToHexString(SHA256.HashData(Stream)).ToLowerInvariant();
        if (Metadata.GetProperty("schema").GetInt32() != 1 || Metadata.GetProperty("platform").GetString() != "IOSSimulator"
            || Metadata.GetProperty("architecture").GetString() != "arm64"
            || Metadata.GetProperty("archive_sha256").GetString() != Hash)
            throw new BuildException("The simulator support archive differs from its manifest.");
        return Archive;
    }
}
