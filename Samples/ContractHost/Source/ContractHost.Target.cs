using UnrealBuildTool;
public class ContractHostTarget : TargetRules
{
    public ContractHostTarget(TargetInfo Target) : base(Target)
    {
        Type = TargetType.Game;
        DefaultBuildSettings = BuildSettingsVersion.Latest;
        IncludeOrderVersion = EngineIncludeOrderVersion.Latest;
        ExtraModuleNames.Add("ContractHost");
    }
}
