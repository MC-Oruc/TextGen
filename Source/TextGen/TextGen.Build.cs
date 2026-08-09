using UnrealBuildTool;

public class TextGen : ModuleRules
{
    public TextGen(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

        PublicDependencyModuleNames.AddRange(
            new[]
            {
                "Core",
                "CoreUObject",
                "Engine",
                "HTTP",
                "Json",
                "JsonUtilities",
                "libzip",
                "DeveloperSettings",
                "ProcessRuntime",
                "Projects",
                "Sockets"
            });

        if (Target.Platform == UnrealTargetPlatform.Win64)
        {
            PublicSystemLibraries.Add("Bcrypt.lib");
        }
    }
}
