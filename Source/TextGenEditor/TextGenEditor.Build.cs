using UnrealBuildTool;

public class TextGenEditor : ModuleRules
{
    public TextGenEditor(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

        PrivateDependencyModuleNames.AddRange(
            new[]
            {
                "Core",
                "CoreUObject",
                "DeveloperSettings",
                "Engine",
                "Slate",
                "SlateCore",
                "UnrealEd",
                "ContentBrowserData",
                "ContentBrowserFileDataSource",
                "TextGen"
            });

        if (Target.Platform == UnrealTargetPlatform.Win64)
        {
            PublicSystemLibraries.Add("Shell32.lib");
        }
    }
}
