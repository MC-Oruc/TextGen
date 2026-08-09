[CmdletBinding()]
param(
    [string]$ModelPath,
    [string]$Tag = "b10333",
    [ValidateSet("CUDA13", "CUDA12", "Vulkan")]
    [string]$Backend = "CUDA13",
    [int]$StartupTimeoutSeconds = 300
)

$ErrorActionPreference = "Stop"

if ([string]::IsNullOrWhiteSpace($ModelPath)) {
    $ModelPath = Join-Path $PSScriptRoot "..\..\..\Content\_SoC\LLM\Models\gemma-4-E2B-it-qat-UD-Q2_K_XL.gguf"
}
$ModelPath = [System.IO.Path]::GetFullPath($ModelPath)
if (-not (Test-Path -LiteralPath $ModelPath -PathType Leaf)) { throw "GGUF model not found: $ModelPath" }

$runtimeRoot = Join-Path $PSScriptRoot "..\..\..\Saved\TextGen\Runtimes\Llamacpp\Win64"
$backendDirectory = switch ($Backend) {
    "CUDA13" { "cuda-13.3" }
    "CUDA12" { "cuda-12.4" }
    "Vulkan" { "vulkan" }
}
$server = Join-Path $runtimeRoot "$backendDirectory\$Tag\llama-server.exe"
if (-not (Test-Path -LiteralPath $server -PathType Leaf)) { throw "llama.cpp runtime not installed: $server" }
$deviceLines = @(& $server --list-devices 2>&1) | Where-Object { $_ -match '^\s*(CUDA|Vulkan)\d+:' }
$deviceLine = @($deviceLines | Where-Object { $_ -notmatch '(?i)Intel' } | Select-Object -First 1)
if ($deviceLine.Count -eq 0) { $deviceLine = @($deviceLines | Select-Object -First 1) }
if ($deviceLine.Count -eq 0) { throw "$Backend runtime did not report an accelerator device." }
$deviceName = ([string]$deviceLine[0]).Split(':', 2)[0].Trim()

$listener = [System.Net.Sockets.TcpListener]::new([System.Net.IPAddress]::Loopback, 0)
$listener.Start()
$port = ([System.Net.IPEndPoint]$listener.LocalEndpoint).Port
$listener.Stop()

$runId = [guid]::NewGuid().ToString("N")
$runDir = Join-Path $PSScriptRoot "..\..\..\Saved\TextGen\IntegrationTests\$runId"
$slotDir = Join-Path $runDir "slots"
$presetPath = Join-Path $runDir "models.ini"
[System.IO.Directory]::CreateDirectory($slotDir) | Out-Null

$modelId = [System.IO.Path]::GetFileNameWithoutExtension($ModelPath) -replace '[\\/:*?"<>|\[\]\s]', '_'
$preset = @(
    "version = 1"
    ""
    "[$modelId]"
    "model = $ModelPath"
    "ctx-size = 4096"
    "device = $deviceName"
    "n-gpu-layers = all"
    "override-tensor = .*exps.*=CPU"
    "mmap = true"
    "flash-attn = on"
    "cache-type-k = f16"
    "cache-type-v = f16"
    "parallel = 1"
    "load-on-startup = false"
    "stop-timeout = 30"
) -join [Environment]::NewLine
[System.IO.File]::WriteAllText($presetPath, $preset, [System.Text.UTF8Encoding]::new($false))

function Invoke-JsonRequest {
    param([string]$Uri, [string]$Method = "Get", [object]$Body)
    $parameters = @{ Uri = $Uri; Method = $Method; TimeoutSec = 300 }
    if ($null -ne $Body) {
        $parameters.ContentType = "application/json"
        $parameters.Body = $Body | ConvertTo-Json -Depth 8 -Compress
    }
    $response = Invoke-WebRequest @parameters
    if ($response.StatusCode -lt 200 -or $response.StatusCode -ge 300) {
        throw "$Method $Uri failed with HTTP $($response.StatusCode)."
    }
    return $response
}

function Wait-ModelStatus {
    param([string]$BaseUrl, [string]$Expected, [datetime]$Deadline)
    do {
        $payload = (Invoke-JsonRequest -Uri "$BaseUrl/models").Content | ConvertFrom-Json
        $entry = @($payload.data) | Where-Object { $_.id -eq $modelId } | Select-Object -First 1
        $status = if ($null -eq $entry) { "unloaded" } else { [string]$entry.status.value }
        if ($status -eq $Expected) { return }
        if ($null -ne $entry -and $entry.status.failed) { throw "Model entered failed state: $($entry.status.error)" }
        Start-Sleep -Milliseconds 250
    } while ([DateTime]::UtcNow -lt $Deadline)
    throw "Model did not reach '$Expected' before timeout."
}

$startInfo = [System.Diagnostics.ProcessStartInfo]::new()
$startInfo.FileName = $server
$startInfo.WorkingDirectory = Split-Path -LiteralPath $server
$startInfo.UseShellExecute = $false
$startInfo.CreateNoWindow = $true
$startInfo.RedirectStandardOutput = $true
$startInfo.RedirectStandardError = $true
@("--host", "127.0.0.1", "--port", "$port", "--models-preset", $presetPath, "--models-max", "1",
  "--no-models-autoload", "--slots", "--slot-save-path", $slotDir, "--no-webui", "--offline") |
    ForEach-Object { [void]$startInfo.ArgumentList.Add($_) }

$process = [System.Diagnostics.Process]::new()
$process.StartInfo = $startInfo
if (-not $process.Start()) { throw "Failed to launch test llama.cpp router." }
$routerProcessId = $process.Id
$stdoutTask = $process.StandardOutput.ReadToEndAsync()
$stderrTask = $process.StandardError.ReadToEndAsync()
$baseUrl = "http://127.0.0.1:$port"

try {
    $deadline = [DateTime]::UtcNow.AddSeconds($StartupTimeoutSeconds)
    do {
        if ($process.HasExited) { throw "llama.cpp router exited with code $($process.ExitCode)." }
        try {
            $health = Invoke-WebRequest -Uri "$baseUrl/health" -SkipHttpErrorCheck -TimeoutSec 5
            if ($health.StatusCode -eq 200) { break }
        } catch { }
        Start-Sleep -Milliseconds 250
    } while ([DateTime]::UtcNow -lt $deadline)
    if ([DateTime]::UtcNow -ge $deadline) { throw "llama.cpp router startup timed out." }

    Invoke-JsonRequest -Uri "$baseUrl/models?reload=1" | Out-Null
    Invoke-JsonRequest -Uri "$baseUrl/models/load" -Method Post -Body @{ model = $modelId } | Out-Null
    Wait-ModelStatus -BaseUrl $baseUrl -Expected "loaded" -Deadline $deadline

    $template = Invoke-JsonRequest -Uri "$baseUrl/apply-template" -Method Post -Body @{
        model = $modelId
        add_generation_prompt = $true
        messages = @(@{ role = "system"; content = "You are a deterministic integration test NPC." })
    }
    $prompt = ([string]($template.Content | ConvertFrom-Json).prompt)
    if ([string]::IsNullOrWhiteSpace($prompt)) { throw "apply-template returned an empty prompt." }

    Invoke-JsonRequest -Uri "$baseUrl/completion" -Method Post -Body @{
        model = $modelId
        prompt = $prompt
        n_predict = 0
        cache_prompt = $true
        id_slot = 0
    } | Out-Null

    $checkpointName = "IntegrationDefault.bin"
    $slotBody = @{ model = $modelId; filename = $checkpointName }
    Invoke-JsonRequest -Uri "$baseUrl/slots/0?action=save" -Method Post -Body $slotBody | Out-Null
    if (-not (Test-Path -LiteralPath (Join-Path $slotDir $checkpointName) -PathType Leaf)) {
        throw "Slot save did not create $checkpointName."
    }
    Invoke-JsonRequest -Uri "$baseUrl/slots/0?action=restore" -Method Post -Body $slotBody | Out-Null

    Invoke-JsonRequest -Uri "$baseUrl/models/unload" -Method Post -Body @{ model = $modelId } | Out-Null
    Wait-ModelStatus -BaseUrl $baseUrl -Expected "unloaded" -Deadline ([DateTime]::UtcNow.AddSeconds($StartupTimeoutSeconds))
    if ($process.HasExited -or $process.Id -ne $routerProcessId) { throw "Router parent process changed during model load/unload." }

    Write-Host "PASS: router PID $routerProcessId stayed alive through load, template prefill, slot save/restore, and unload."
    Write-Host "Runtime: $Tag  Backend: $Backend  Device: $deviceName  Model: $modelId  Port: $port"
} finally {
    if (-not $process.HasExited) {
        $process.Kill($true)
        $process.WaitForExit(10000) | Out-Null
    }
    [System.IO.File]::WriteAllText((Join-Path $runDir "stdout.log"), $stdoutTask.GetAwaiter().GetResult())
    [System.IO.File]::WriteAllText((Join-Path $runDir "stderr.log"), $stderrTask.GetAwaiter().GetResult())
    $process.Dispose()
}
