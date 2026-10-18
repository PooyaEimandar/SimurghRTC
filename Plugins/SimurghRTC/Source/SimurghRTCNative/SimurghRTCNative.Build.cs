using System;
using System.Collections.Generic;
using System.IO;
using System.Security.Cryptography;
using System.Text.Json;
using UnrealBuildTool;

public class SimurghRTCNative : SimurghRTCModuleRules
{
    public SimurghRTCNative(ReadOnlyTargetRules Target) : base(Target)
    {
        PublicDependencyModuleNames.AddRange(new string[] { "Core", "SimurghRTCCore" });
        bool UseEngineWebRTC = Target.Platform == UnrealTargetPlatform.Mac
            || Target.Platform == UnrealTargetPlatform.Win64
            || (Target.Platform == UnrealTargetPlatform.Linux && Target.Architecture == UnrealArch.X64);
        bool UseMobileWebRTC = Target.Platform == UnrealTargetPlatform.Android || Target.Platform == UnrealTargetPlatform.IOS;
        PrivateDefinitions.Add("SIMURGHRTC_WITH_ENGINE_WEBRTC=" + (UseEngineWebRTC ? "1" : "0"));
        PrivateDefinitions.Add("SIMURGHRTC_WITH_WEBRTC=" + (UseEngineWebRTC || UseMobileWebRTC ? "1" : "0"));
        if (UseEngineWebRTC || UseMobileWebRTC)
        {
            if (Target.Version.MajorVersion != 5 || Target.Version.MinorVersion != 8 || Target.Version.PatchVersion != 3)
                throw new BuildException("The SimurghRTC native adapter currently targets Unreal Engine 5.8.3. Review the adapter before using another version.");
        }
        if (UseEngineWebRTC)
        {
            // Explicitly include archive dependencies: precompiled engine external modules may not propagate private links.
            AddEngineThirdPartyPrivateStaticDependencies(Target, "WebRTC", "OpenSSL", "libOpus", "LibVpx");
            if (Target.Platform == UnrealTargetPlatform.Mac)
                PublicFrameworks.AddRange(new string[] { "AudioToolbox", "CoreAudio", "CoreVideo", "VideoToolbox" });
        }
        if (UseMobileWebRTC)
        {
            bool IsSimulator = Target.Platform == UnrealTargetPlatform.IOS && Target.Architecture == UnrealArch.IOSSimulator;
            if (Target.Type == TargetType.Server || (Target.Architecture != UnrealArch.Arm64 && !IsSimulator))
                throw new BuildException("SimurghRTC mobile artifacts require ARM64 device clients or the explicit iOS ARM64 simulator target.");
            string ArtifactPlatform = IsSimulator ? "IOSSimulator" : Target.Platform.ToString();
            string Root = Path.Combine(PluginDirectory, "ThirdParty", "WebRTC", ArtifactPlatform, "arm64");
            try
            {
                ExternalDependencies.AddRange(SimurghRTCMobileArtifact.Validate(Root, ArtifactPlatform));
                using FileStream ManifestStream = File.OpenRead(Path.Combine(Root, "artifact.json"));
                string ManifestHash = Convert.ToHexString(SHA256.HashData(ManifestStream)).ToLowerInvariant();
                PrivateDefinitions.Add("SIMURGHRTC_MOBILE_ARTIFACT_SHA256=\"" + ManifestHash + "\"");
            }
            catch (Exception Error)
            {
                throw new BuildException("SimurghRTC mobile artifact rejected: " + Error.Message
                    + " Stage a checked artifact with tools/prepare_profile.py --mobile-artifact. See docs/MOBILE_ARTIFACTS.md.");
            }
            // This path never requests Unreal's WebRTC module or its Android Java payload.
            PrivateIncludePaths.Add(Path.Combine(Root, "Include"));
            PrivateIncludePaths.Add(Path.Combine(Root, "Include", "third_party", "abseil-cpp"));
            PrivateIncludePaths.Add(Path.Combine(Root, "Include", "third_party", "libyuv", "include"));
            PublicAdditionalLibraries.Add(Path.Combine(Root, "Lib", "libwebrtc.a"));
            PrivateDefinitions.AddRange(new string[] { "WEBRTC_5414=1", "WEBRTC_POSIX=1", "ABSL_ALLOCATOR_NOTHROW=1" });
            AddEngineThirdPartyPrivateStaticDependencies(Target, "OpenSSL", "libOpus");
            if (IsSimulator)
                PublicAdditionalLibraries.Add(Path.Combine(Root, "Lib", "libvpx.a"));
            else
                AddEngineThirdPartyPrivateStaticDependencies(Target, "LibVpx");
            if (Target.Platform == UnrealTargetPlatform.Android)
            {
                PrivateDefinitions.Add("WEBRTC_ANDROID=1");
                PublicSystemLibraries.AddRange(new string[] { "log", "android" });
            }
            else
            {
                PrivateDefinitions.AddRange(new string[] { "WEBRTC_MAC=1", "WEBRTC_IOS=1" });
                PublicFrameworks.AddRange(new string[] { "AudioToolbox", "CoreAudio", "CoreMedia", "CoreVideo", "VideoToolbox" });
            }
        }
    }
}

// Kept in a rules file so both normal and staged plugins compile the same validator.
// The packaging tool also checks archive members. UBT rechecks every payload hash on rule evaluation.
public static class SimurghRTCMobileArtifact
{
    public static readonly string[] RequiredFiles = new string[] {
        "Lib/libwebrtc.a", "Include/api/create_peerconnection_factory.h", "Include/rtc_base/thread.h",
        "Include/third_party/abseil-cpp/absl/base/config.h", "Include/third_party/libyuv/include/libyuv.h",
        "Build/args.gn", "Build/source-lock.json", "Build/recipe.txt", "Licenses/THIRD_PARTY_NOTICES.txt"
    };

    static void Require(bool Condition, string Message)
    {
        if (!Condition) throw new InvalidDataException(Message);
    }

    static void CheckObject(JsonElement Value)
    {
        if (Value.ValueKind == JsonValueKind.Object)
        {
            HashSet<string> Names = new HashSet<string>(StringComparer.Ordinal);
            foreach (JsonProperty Property in Value.EnumerateObject())
            {
                Require(Names.Add(Property.Name), "Duplicate JSON key: " + Property.Name);
                CheckObject(Property.Value);
            }
        }
        else if (Value.ValueKind == JsonValueKind.Array)
            foreach (JsonElement Item in Value.EnumerateArray()) CheckObject(Item);
    }

    static void CheckPath(string Name)
    {
        Require(System.Text.RegularExpressions.Regex.IsMatch(Name, @"\A[A-Za-z0-9_./+\-]+\z")
            && !Name.StartsWith("/"), "Invalid relative artifact path: " + Name);
        foreach (string Part in Name.Split('/'))
            Require(Part != "" && Part != "." && Part != "..", "Unsafe artifact path: " + Name);
    }

    static void CollectFiles(string Root, string DirectoryPath, Dictionary<string, string> Files, HashSet<string> Names)
    {
        Require((File.GetAttributes(DirectoryPath) & FileAttributes.ReparsePoint) == 0, "Artifact symlinks are not supported.");
        foreach (string Entry in Directory.EnumerateFileSystemEntries(DirectoryPath))
        {
            string Name = Path.GetRelativePath(Root, Entry).Replace('\\', '/');
            CheckPath(Name);
            Require(Names.Add(Name), "Case-colliding artifact path: " + Name);
            FileAttributes Attributes = File.GetAttributes(Entry);
            Require((Attributes & FileAttributes.ReparsePoint) == 0, "Artifact symlinks are not supported.");
            if ((Attributes & FileAttributes.Directory) != 0) CollectFiles(Root, Entry, Files, Names);
            else if (Name != "artifact.json") Files.Add(Name, Entry);
        }
    }

    public static List<string> Validate(string Root, string Platform)
    {
        Require(Platform == "Android" || Platform == "IOS" || Platform == "IOSSimulator", "Unsupported mobile platform.");
        string Manifest = Path.Combine(Root, "artifact.json");
        Require(File.Exists(Manifest), "Missing " + Manifest);
        Dictionary<string, string> Files = new Dictionary<string, string>(StringComparer.Ordinal);
        CollectFiles(Root, Root, Files, new HashSet<string>(StringComparer.OrdinalIgnoreCase));
        using FileStream ManifestStream = File.OpenRead(Manifest);
        // UE's rules compiler omits System.Memory; the stream-only API avoids its Parse overloads.
        using JsonDocument Document = JsonDocument.ParseAsync(ManifestStream).GetAwaiter().GetResult();
        JsonElement Top = Document.RootElement;
        CheckObject(Top);
        JsonElement Meta = Top.GetProperty("metadata");
        Require(Meta.GetProperty("schema").GetInt32() == 1, "Unsupported artifact schema.");
        foreach (var Pair in new Dictionary<string, string> {
            { "platform", Platform }, { "architecture", "arm64" }, { "engine_version", "5.8.3" },
            { "adapter", "webrtc-5414-ue5.8.3-v1" }, { "configuration", "Release" },
            { "dependencies", Platform == "IOSSimulator" ? "unreal-openssl-opus-simulator-vpx" : "unreal-openssl-opus-vpx" }
        }) Require(Meta.GetProperty(Pair.Key).GetString() == Pair.Value, "Incompatible artifact " + Pair.Key);
        Require(Meta.GetProperty("toolchain").GetProperty("cpp_runtime").GetString() == "libc++", "Incompatible C++ runtime.");
        if (Platform == "Android")
            Require(Meta.GetProperty("toolchain").TryGetProperty("cpp_abi", out JsonElement ABI)
                && ABI.GetString() == "itanium", "Rebuild Android WebRTC with the standard Itanium C++ ABI; relative vtables are incompatible.");
        foreach (string Required in RequiredFiles)
            Require(Files.ContainsKey(Required) && new FileInfo(Files[Required]).Length > 0, "Missing or empty " + Required);
        if (Platform == "IOSSimulator")
            foreach (string Required in new[] { "Lib/libvpx.a", "Build/vpx-build.json", "Licenses/VPX-LICENSE.txt" })
                Require(Files.ContainsKey(Required) && new FileInfo(Files[Required]).Length > 0, "Missing or empty " + Required);
        JsonElement Inventory = Top.GetProperty("files");
        int Count = 0;
        List<string> Dependencies = new List<string> { Manifest };
        foreach (JsonProperty Entry in Inventory.EnumerateObject())
        {
            CheckPath(Entry.Name);
            Require(Files.ContainsKey(Entry.Name), "Missing artifact file: " + Entry.Name);
            string FilePath = Files[Entry.Name];
            string Extension = Path.GetExtension(FilePath).ToLowerInvariant();
            Require(!new HashSet<string> { ".a", ".so", ".dylib", ".dll", ".lib", ".jar", ".aar" }.Contains(Extension)
                || Entry.Name == "Lib/libwebrtc.a" || (Platform == "IOSSimulator" && Entry.Name == "Lib/libvpx.a"), "Unexpected binary payload: " + Entry.Name);
            using FileStream Stream = File.OpenRead(FilePath);
            string Actual = Convert.ToHexString(SHA256.HashData(Stream)).ToLowerInvariant();
            Require(Actual == Entry.Value.GetString(), "SHA256 mismatch: " + Entry.Name);
            Dependencies.Add(FilePath);
            ++Count;
        }
        Require(Count == Files.Count, "Artifact inventory differs from disk.");
        return Dependencies;
    }
}
