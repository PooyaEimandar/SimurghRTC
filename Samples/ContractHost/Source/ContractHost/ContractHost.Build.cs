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
            ExternalDependencies.Add(Support); // Revalidate hashes when an existing export changes.
            ExternalDependencies.Add(Path.Combine(Path.GetDirectoryName(Support)!, "support.json"));
            ExternalDependencies.Add(Path.Combine(Path.GetDirectoryName(Support)!, "source-lock.json"));
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
        // Pin the reviewed source lock as well as the self-described archive hash.
        const string SourceLockHash = "28f7039546b941e68c4b6120a848c8a5be1d897c670f9ef0c8ef57ffb8268f0d";
        string SourceLock = Path.Combine(Root, "source-lock.json");
        if (!File.Exists(SourceLock))
            throw new BuildException("Simulator support lacks the reviewed security source lock.");
        using FileStream LockStream = File.OpenRead(SourceLock);
        string ActualLockHash = Convert.ToHexString(SHA256.HashData(LockStream)).ToLowerInvariant();
        using FileStream Stream = File.OpenRead(Archive);
        string Hash = Convert.ToHexString(SHA256.HashData(Stream)).ToLowerInvariant();
        if (Metadata.GetProperty("schema").GetInt32() != 2 || Metadata.GetProperty("platform").GetString() != "IOSSimulator"
            || Metadata.GetProperty("architecture").GetString() != "arm64"
            || Metadata.GetProperty("archive_sha256").GetString() != Hash
            || Metadata.GetProperty("version").GetString() != "1.70.0"
            || Metadata.GetProperty("source_lock_sha256").GetString() != SourceLockHash
            || ActualLockHash != SourceLockHash)
            throw new BuildException("Simulator support is changed or retired. Rebuild with the current security lock.");
        return Archive;
    }
}
