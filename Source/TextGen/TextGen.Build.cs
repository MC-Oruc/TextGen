using UnrealBuildTool;
using EpicGames.Core;
using System;
using System.IO;

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

            if (!Target.bBuildEditor && Target.ProjectFile != null)
            {
                StageInstalledLlamacppRuntimes(Target);
            }
        }
    }

    private void StageInstalledLlamacppRuntimes(ReadOnlyTargetRules Target)
    {
        ConfigHierarchy GameConfig = ConfigCache.ReadHierarchy(
            ConfigHierarchyType.Game,
            DirectoryReference.FromFile(Target.ProjectFile),
            Target.Platform);
        const string SettingsSection = "/Script/TextGen.TextGenProjectSettings";
        string RuntimeTag = GameConfig.GetStructEntryForSetting(
            SettingsSection, "PackagedDefaults", "RuntimeTag") ?? string.Empty;
        if (string.IsNullOrWhiteSpace(RuntimeTag))
        {
            return;
        }

        string RuntimeRoot = Path.Combine(Target.ProjectFile.Directory.FullName, "Saved", "TextGen", "Runtimes",
            "Llamacpp", "Win64");
        foreach (string BackendDirectory in new[] { "cuda-13.3", "cuda-12.4", "vulkan" })
        {
            string RuntimeDirectory = Path.Combine(RuntimeRoot, BackendDirectory, RuntimeTag);
            ValidateRuntimeInventory(RuntimeDirectory, BackendDirectory, RuntimeTag);
            foreach (string SourceFile in Directory.GetFiles(RuntimeDirectory, "*", SearchOption.AllDirectories))
            {
                string RelativeFile = SourceFile.Substring(RuntimeDirectory.Length)
                    .TrimStart(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar);
                string StagedFile = Path.Combine("$(TargetOutputDir)", "TextGen", "Runtimes", "Llamacpp", "Win64",
                    BackendDirectory, RuntimeTag, RelativeFile);
                RuntimeDependencies.Add(StagedFile, SourceFile, StagedFileType.NonUFS);
            }
        }
    }

    private static void ValidateRuntimeInventory(string RuntimeDirectory, string BackendDirectory, string RuntimeTag)
    {
        if (!File.Exists(Path.Combine(RuntimeDirectory, "llama-server.exe")))
        {
            throw new BuildException(
                $"TextGen {BackendDirectory} llama.cpp runtime {RuntimeTag} is not installed. Open the editor and wait for automatic runtime preparation before packaging.");
        }
        if (BackendDirectory == "vulkan")
        {
            if (!File.Exists(Path.Combine(RuntimeDirectory, "ggml-vulkan.dll")))
            {
                throw new BuildException($"TextGen Vulkan runtime {RuntimeTag} is incomplete: {RuntimeDirectory}");
            }
            return;
        }
        if (Directory.GetFiles(RuntimeDirectory, "cudart*.dll").Length == 0
            || Directory.GetFiles(RuntimeDirectory, "cublas*.dll").Length == 0)
        {
            throw new BuildException($"TextGen {BackendDirectory} runtime {RuntimeTag} has incomplete CUDA dependencies: {RuntimeDirectory}");
        }
    }
}
