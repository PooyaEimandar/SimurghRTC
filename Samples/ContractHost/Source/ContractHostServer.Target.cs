using UnrealBuildTool;
public class ContractHostServerTarget : TargetRules
{
    public ContractHostServerTarget(TargetInfo Target) : base(Target)
    {
        Type = TargetType.Server;
        DefaultBuildSettings = BuildSettingsVersion.Latest;
        IncludeOrderVersion = EngineIncludeOrderVersion.Latest;
        ExtraModuleNames.Add("ContractHost");
    }
}
