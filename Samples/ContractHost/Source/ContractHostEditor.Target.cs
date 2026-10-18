using UnrealBuildTool;
public class ContractHostEditorTarget : TargetRules
{
    public ContractHostEditorTarget(TargetInfo Target) : base(Target)
    {
        Type = TargetType.Editor;
        DefaultBuildSettings = BuildSettingsVersion.Latest;
        IncludeOrderVersion = EngineIncludeOrderVersion.Latest;
        ExtraModuleNames.Add("ContractHost");
    }
}
