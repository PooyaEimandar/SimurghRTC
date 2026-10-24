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
        PrivateDependencyModuleNames.AddRange(new string[] { "CoreUObject", "Engine" });
        bool IsSimulator = Target.Platform == UnrealTargetPlatform.IOS && Target.Architecture == UnrealArch.IOSSimulator;
        bool Supported = (Target.Platform == UnrealTargetPlatform.Mac || Target.Platform == UnrealTargetPlatform.Android
            || Target.Platform == UnrealTargetPlatform.IOS) && (Target.Architecture == UnrealArch.Arm64 || IsSimulator);
        PrivateDefinitions.Add("SIMURGHRTC_WITH_ENGINE_WEBRTC=0");
        PrivateDefinitions.Add("SIMURGHRTC_WITH_WEBRTC=" + (Supported ? "1" : "0"));
        if (!Supported) return; // Other host adapters remain unverified; never fall back to engine RTC libraries.
        if (Target.Version.MajorVersion != 5 || Target.Version.MinorVersion != 8 || Target.Version.PatchVersion != 3)
            throw new BuildException("The SimurghRTC native adapter requires Unreal Engine 5.8.3.");
        if (Target.Type == TargetType.Server && Target.Platform != UnrealTargetPlatform.Mac)
            throw new BuildException("Mobile Server targets are not supported.");
        string Platform = IsSimulator ? "IOSSimulator" : Target.Platform.ToString();
        string Root = Path.Combine(PluginDirectory, "ThirdParty", "WebRTC", Platform, "arm64");
        try
        {
            ExternalDependencies.AddRange(SimurghRTCMobileArtifact.ValidateIsolated(Root, Platform,
                Path.Combine(ModuleDirectory, "NativeBackend.inputs.json")));
            using FileStream Stream = File.OpenRead(Path.Combine(Root, "artifact.json"));
            string Hash = Convert.ToHexString(SHA256.HashData(Stream)).ToLowerInvariant();
            PrivateDefinitions.Add("SIMURGHRTC_NATIVE_ARTIFACT_SHA256=\"" + Hash + "\"");
        }
        catch (Exception Error)
        {
            throw new BuildException("SimurghRTC isolated artifact rejected: " + Error.Message
                + " Build/stage M155 with tools/build_native_backend.py and tools/prepare_profile.py --native-artifact.");
        }
        PrivateIncludePaths.Add(Path.Combine(Root, "Include"));
        PublicAdditionalLibraries.Add(Path.Combine(Root, "Lib", "libwebrtc.a"));
        if (Target.Platform == UnrealTargetPlatform.Android)
            PublicSystemLibraries.AddRange(new string[] { "log", "android" });
        else
            PublicFrameworks.AddRange(new string[] { "Foundation", "Security", "SystemConfiguration", "AudioToolbox", "CoreAudio" });
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

    public static List<string> ValidateIsolated(string Root, string Platform, string ExpectedPath)
    {
        Dictionary<string, string> Files = new Dictionary<string, string>(StringComparer.Ordinal);
        CollectFiles(Root, Root, Files, new HashSet<string>(StringComparer.OrdinalIgnoreCase));
        string Manifest = Path.Combine(Root, "artifact.json");
        using FileStream Stream = File.OpenRead(Manifest);
        using JsonDocument Document = JsonDocument.ParseAsync(Stream).GetAwaiter().GetResult();
        using FileStream ExpectedStream = File.OpenRead(ExpectedPath);
        using JsonDocument Expected = JsonDocument.ParseAsync(ExpectedStream).GetAwaiter().GetResult();
        JsonElement Top = Document.RootElement, Pins = Expected.RootElement;
        CheckObject(Top);
        JsonElement Meta = Top.GetProperty("metadata");
        Require(Meta.GetProperty("schema").GetInt32() == 2, "Rebuild the retired M109 artifact using the M155 C ABI.");
        foreach (var Pair in new Dictionary<string, string> {
            { "platform", Platform }, { "architecture", "arm64" }, { "engine_version", "5.8.3" },
            { "adapter", "webrtc-m155-cabi-v1" }, { "configuration", "Release" },
            { "dependencies", "isolated-boringssl-opus-vpx" },
            { "source_revision", Pins.GetProperty("source_revision").GetString()! },
            { "abi_header_sha256", Pins.GetProperty("abi_header_sha256").GetString()! }
        }) Require(Meta.GetProperty(Pair.Key).GetString() == Pair.Value, "Incompatible native " + Pair.Key);
        foreach (string Required in new[] { "Lib/libwebrtc.a", "Include/simurgh_rtc_abi.h", "Build/source-lock.json",
            "Build/args.gn", "Build/build.json", "Licenses/THIRD_PARTY_NOTICES.txt" })
            Require(Files.ContainsKey(Required) && new FileInfo(Files[Required]).Length > 0, "Missing or empty " + Required);
        List<string> Dependencies = new List<string> { Manifest, ExpectedPath };
        JsonElement Inventory = Top.GetProperty("files");
        int Count = 0;
        foreach (JsonProperty Entry in Inventory.EnumerateObject())
        {
            CheckPath(Entry.Name);
            Require(Files.ContainsKey(Entry.Name), "Missing native file: " + Entry.Name);
            string PathValue = Files[Entry.Name];
            string Extension = Path.GetExtension(PathValue).ToLowerInvariant();
            Require(!new HashSet<string> { ".a", ".o", ".so", ".dylib", ".dll", ".lib", ".jar", ".aar" }.Contains(Extension)
                || Entry.Name == "Lib/libwebrtc.a", "Unexpected native binary: " + Entry.Name);
            using FileStream Payload = File.OpenRead(PathValue);
            string Hash = Convert.ToHexString(SHA256.HashData(Payload)).ToLowerInvariant();
            Require(Hash == Entry.Value.GetString(), "SHA256 mismatch: " + Entry.Name);
            Dependencies.Add(PathValue); ++Count;
        }
        Require(Count == Files.Count, "Native inventory differs from disk.");
        Require(Inventory.GetProperty("Include/simurgh_rtc_abi.h").GetString() == Pins.GetProperty("abi_header_sha256").GetString(), "ABI header drift.");
        Require(Inventory.GetProperty("Build/source-lock.json").GetString() == Pins.GetProperty(Platform == "Android" ? "android_source_lock_sha256" : "source_lock_sha256").GetString(), "Native source lock drift.");
        using FileStream BuildStream = File.OpenRead(Files["Build/build.json"]);
        using JsonDocument Build = JsonDocument.ParseAsync(BuildStream).GetAwaiter().GetResult();
        CheckObject(Build.RootElement);
        foreach (JsonProperty Source in Pins.GetProperty("bridge_sources").EnumerateObject())
            Require(Build.RootElement.GetProperty("bridge_sources").GetProperty(Source.Name).GetString() == Source.Value.GetString(), "Native bridge drift: " + Source.Name);
        if (Platform == "Android")
        {
            Require(Files.ContainsKey("Build/link-probe.json"), "Android artifact lacks its NDK consumer link check.");
            using FileStream LinkStream = File.OpenRead(Files["Build/link-probe.json"]);
            using JsonDocument Link = JsonDocument.ParseAsync(LinkStream).GetAwaiter().GetResult();
            CheckObject(Link.RootElement);
            JsonElement Proof = Link.RootElement;
            Require(Proof.GetProperty("passed").GetBoolean() && Proof.GetProperty("minimum_api").GetInt32() == 26
                && Proof.GetProperty("load_alignment").GetInt32() == 16384
                && Build.RootElement.GetProperty("ndk").GetString() == "27.2.12479018"
                && Proof.GetProperty("compiler").GetString()!.Contains("clang version 18.0.3"), "Invalid Android NDK consumer check.");
            string RuntimeHash = Proof.GetProperty("compiler_runtime_sha256").GetString()!;
            Require(System.Text.RegularExpressions.Regex.IsMatch(RuntimeHash, @"\A[0-9a-f]{64}\z")
                && RuntimeHash == Build.RootElement.GetProperty("compiler_runtime_sha256").GetString(), "Android compiler runtime identity mismatch.");
        }
        return Dependencies;
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
