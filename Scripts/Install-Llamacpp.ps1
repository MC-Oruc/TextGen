[CmdletBinding()]
param(
    [string]$Tag,
    [ValidateSet("CUDA13", "CUDA12", "Vulkan")]
    [string]$Backend = "CUDA13",
    [ValidateSet("Runtime", "CudaDependencies")]
    [string]$Component = "Runtime",
    [string]$InstallRoot = (Join-Path $PSScriptRoot "..\..\..\Saved\TextGen\Runtimes\Llamacpp\Win64")
)

$ErrorActionPreference = "Stop"
$repository = "ggml-org/llama.cpp"
$headers = @{ "Accept" = "application/vnd.github+json"; "X-GitHub-Api-Version" = "2022-11-28" }
$backendDirectory = switch ($Backend) {
    "CUDA13" { "cuda-13.3" }
    "CUDA12" { "cuda-12.4" }
    "Vulkan" { "vulkan" }
}
if ($Backend -eq "Vulkan" -and $Component -eq "CudaDependencies") {
    throw "Vulkan has no CUDA dependency component."
}

$releaseUri = if ([string]::IsNullOrWhiteSpace($Tag)) {
    "https://api.github.com/repos/$repository/releases/latest"
} else {
    "https://api.github.com/repos/$repository/releases/tags/$([uri]::EscapeDataString($Tag))"
}
$release = Invoke-RestMethod -Uri $releaseUri -Headers $headers
$resolvedTag = [string]$release.tag_name
$runtimeAssetName = if ($Backend -eq "Vulkan") {
    "llama-$resolvedTag-bin-win-vulkan-x64.zip"
} else {
    "llama-$resolvedTag-bin-win-$backendDirectory-x64.zip"
}
$dependencyAssetName = if ($Backend -eq "Vulkan") { $null } else { "cudart-llama-bin-win-$backendDirectory-x64.zip" }

function Resolve-ReleaseAsset([string]$Name) {
    $assetMatches = @($release.assets | Where-Object { $_.name -ceq $Name })
    if ($assetMatches.Count -ne 1) {
        throw "Release '$resolvedTag' does not contain exactly one '$Name' asset."
    }
    $digest = [string]$assetMatches[0].digest
    if ($digest -notmatch '^sha256:([0-9a-fA-F]{64})$') {
        throw "GitHub asset '$Name' has no mandatory SHA-256 digest."
    }
    [pscustomobject]@{
        Name = $Name
        Url = [string]$assetMatches[0].browser_download_url
        Sha256 = $Matches[1].ToLowerInvariant()
    }
}

function Download-VerifiedAsset($Asset, [string]$Destination) {
    Invoke-WebRequest -Uri $Asset.Url -OutFile $Destination -Headers $headers
    $actual = (Get-FileHash -LiteralPath $Destination -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($actual -ne $Asset.Sha256) {
        throw "SHA-256 mismatch for '$($Asset.Name)'. Expected $($Asset.Sha256), got $actual."
    }
}

function Expand-SafeArchive([string]$Archive, [string]$Destination) {
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $zip = [System.IO.Compression.ZipFile]::OpenRead($Archive)
    try {
        $prefix = [System.IO.Path]::GetFullPath($Destination + [System.IO.Path]::DirectorySeparatorChar)
        foreach ($entry in $zip.Entries) {
            $entryPath = [System.IO.Path]::GetFullPath((Join-Path $Destination $entry.FullName))
            if (-not $entryPath.StartsWith($prefix, [System.StringComparison]::OrdinalIgnoreCase)) {
                throw "Unsafe archive entry rejected: $($entry.FullName)"
            }
        }
    } finally {
        $zip.Dispose()
    }
    [System.IO.Compression.ZipFile]::ExtractToDirectory($Archive, $Destination)
}

$root = [System.IO.Path]::GetFullPath($InstallRoot)
$target = Join-Path (Join-Path $root $backendDirectory) $resolvedTag
$dependencyCache = Join-Path (Join-Path $root "_Dependencies") $backendDirectory
[System.IO.Directory]::CreateDirectory($root) | Out-Null
$runtimeAsset = if ($Component -eq "Runtime") { Resolve-ReleaseAsset $runtimeAssetName } else { $null }
if ($Component -eq "Runtime" -and (Test-Path -LiteralPath $target)) {
    Write-Host "llama.cpp $resolvedTag ($backendDirectory) is already installed at $target"
    exit 0
}
$staging = Join-Path $root (".staging-{0}-{1}-{2}" -f $backendDirectory, $resolvedTag, [guid]::NewGuid().ToString('N'))
[System.IO.Directory]::CreateDirectory($staging) | Out-Null

$dependencyAsset = if ($dependencyAssetName) { Resolve-ReleaseAsset $dependencyAssetName } else { $null }
if ($Component -eq "CudaDependencies") {
    $dependencyArchive = Join-Path $staging $dependencyAsset.Name
    $dependencyExtract = Join-Path $staging "dependency"
    [System.IO.Directory]::CreateDirectory($dependencyExtract) | Out-Null
    Download-VerifiedAsset $dependencyAsset $dependencyArchive
    Expand-SafeArchive $dependencyArchive $dependencyExtract
    [System.IO.Directory]::CreateDirectory($dependencyCache) | Out-Null
    Get-ChildItem -LiteralPath $dependencyExtract -Recurse -File -Filter "*.dll" | ForEach-Object {
        $dependencyFile = $_
        Copy-Item -LiteralPath $dependencyFile.FullName -Destination (Join-Path $dependencyCache $dependencyFile.Name) -Force
        Get-ChildItem -LiteralPath (Join-Path $root $backendDirectory) -Directory -ErrorAction SilentlyContinue | ForEach-Object {
            $runtimeDirectory = $_
            Copy-Item -LiteralPath $dependencyFile.FullName -Destination (Join-Path $runtimeDirectory.FullName $dependencyFile.Name) -Force
        }
    }
    Remove-Item -LiteralPath $staging -Recurse -Force
    Write-Host "Updated llama.cpp $backendDirectory dependencies from $resolvedTag."
    exit 0
}

$runtimeArchive = Join-Path $staging $runtimeAsset.Name
$runtimeExtract = Join-Path $staging "runtime"
[System.IO.Directory]::CreateDirectory($runtimeExtract) | Out-Null
Download-VerifiedAsset $runtimeAsset $runtimeArchive
Expand-SafeArchive $runtimeArchive $runtimeExtract
$server = @(Get-ChildItem -LiteralPath $runtimeExtract -Recurse -File -Filter "llama-server.exe")
if ($server.Count -ne 1) {
    throw "Package must contain exactly one llama-server.exe. Staging retained at $staging"
}
$packageRoot = $server[0].Directory.FullName
$publishRoot = Join-Path $staging "publish"
[System.IO.Directory]::CreateDirectory($publishRoot) | Out-Null
Copy-Item -LiteralPath $server[0].FullName -Destination $publishRoot
Get-ChildItem -LiteralPath $packageRoot -File -Filter "*.dll" | ForEach-Object {
    Copy-Item -LiteralPath $_.FullName -Destination $publishRoot
}

if ($dependencyAsset) {
    if (-not (Test-Path -LiteralPath $dependencyCache)) {
        $dependencyArchive = Join-Path $staging $dependencyAsset.Name
        $dependencyExtract = Join-Path $staging "dependency"
        [System.IO.Directory]::CreateDirectory($dependencyExtract) | Out-Null
        Download-VerifiedAsset $dependencyAsset $dependencyArchive
        Expand-SafeArchive $dependencyArchive $dependencyExtract
        [System.IO.Directory]::CreateDirectory($dependencyCache) | Out-Null
        Get-ChildItem -LiteralPath $dependencyExtract -Recurse -File -Filter "*.dll" | ForEach-Object {
            Copy-Item -LiteralPath $_.FullName -Destination (Join-Path $dependencyCache $_.Name)
        }
    }
    Get-ChildItem -LiteralPath $dependencyCache -File -Filter "*.dll" | ForEach-Object {
        Copy-Item -LiteralPath $_.FullName -Destination (Join-Path $publishRoot $_.Name)
    }
}

$inventory = @(Get-ChildItem -LiteralPath $publishRoot -File | Sort-Object Name | ForEach-Object {
    [ordered]@{ name = $_.Name; size = $_.Length; sha256 = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant() }
})
$manifest = [ordered]@{
    tag = $resolvedTag
    platform = "Win64"
    architecture = "x64"
    backend = $backendDirectory
    runtime_asset = $runtimeAsset.Name
    runtime_sha256 = $runtimeAsset.Sha256
    cuda_dependency_asset = if ($dependencyAsset) { $dependencyAsset.Name } else { $null }
    installed_utc = [DateTime]::UtcNow.ToString("o")
    files = $inventory
}
$manifest | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $publishRoot "runtime-manifest.json") -Encoding utf8NoBOM
[System.IO.Directory]::CreateDirectory((Split-Path -Parent $target)) | Out-Null
Move-Item -LiteralPath $publishRoot -Destination $target
Remove-Item -LiteralPath $staging -Recurse -Force
Write-Host "Installed llama.cpp $resolvedTag ($backendDirectory) to $target"
