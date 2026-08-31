<#
  Heavy manual integration test for persistent llama.cpp slot caches.

  This test intentionally stays outside Unreal Automation discovery. It loads a real
  GGUF, performs multiple prompt prefills, saves/restores a .bin slot checkpoint,
  and fails unless the restored checkpoint reduces both processed prompt tokens and
  prompt evaluation time.
#>
[CmdletBinding()]
param(
    [string]$ModelPath,
    [string]$Tag,
    [ValidateSet("CUDA13", "CUDA12", "Vulkan")]
    [string]$Backend = "CUDA13",
    [int]$StartupTimeoutSeconds = 300,
    [ValidateRange(8, 96)]
    [int]$CommonPromptRepeats = 32,
    [ValidateRange(0.50, 0.99)]
    [double]$MinimumCacheHitRatio = 0.80,
    [ValidateRange(0.05, 0.75)]
    [double]$MaximumWarmPromptRatio = 0.25,
    [ValidateRange(0.05, 0.90)]
    [double]$MaximumWarmTimeRatio = 0.50,
    [switch]$DirectServer,
    [switch]$UseTokenPrompts,
    [bool]$SwaFull = $true,
    [ValidateRange(0, 256)]
    [int]$Threads = 0,
    [ValidateRange(1, 8192)]
    [int]$BatchSize = 2048,
    [ValidateRange(1, 8192)]
    [int]$UBatchSize = 512,
    [ValidateSet("on", "off", "auto")]
    [string]$FlashAttention = "on",
    [bool]$ContinuousBatching = $false,
    [ValidateRange(-2, 65536)]
    [int]$CacheRamMiB = -2,
    [ValidateRange(0, 100)]
    [int]$Poll = 0
)

$ErrorActionPreference = "Stop"

if ([string]::IsNullOrWhiteSpace($ModelPath)) {
    $ModelPath = Join-Path $PSScriptRoot "..\..\..\Content\_SoC\LLM\Models\gemma-4-E2B-it-qat-UD-Q2_K_XL.gguf"
}
$ModelPath = [System.IO.Path]::GetFullPath($ModelPath)
if (-not (Test-Path -LiteralPath $ModelPath -PathType Leaf)) { throw "GGUF model not found: $ModelPath" }

$runtimeRoot = Join-Path $PSScriptRoot "..\..\..\Saved\TextGen\Runtimes\Llamacpp\Win64"
if ([string]::IsNullOrWhiteSpace($Tag)) { $Tag = "v0.3.0" }
$backendDirectory = switch ($Backend) {
    "CUDA13" { "cuda-13.3" }
    "CUDA12" { "cuda-12.4" }
    "Vulkan" { "vulkan" }
}
$server = Join-Path $runtimeRoot "$backendDirectory\$Tag\llama-server.exe"
if (-not (Test-Path -LiteralPath $server -PathType Leaf)) { throw "llama.cpp runtime not installed: $server" }
if ($Threads -le 0) {
    $Threads = [Math]::Max(1, [int](Get-CimInstance Win32_Processor | Measure-Object -Property NumberOfCores -Sum).Sum)
}
if ($UBatchSize -gt $BatchSize) { throw "UBatchSize cannot exceed BatchSize." }

$listener = [System.Net.Sockets.TcpListener]::new([System.Net.IPAddress]::Loopback, 0)
$listener.Start()
$port = ([System.Net.IPEndPoint]$listener.LocalEndpoint).Port
$listener.Stop()

$runId = [guid]::NewGuid().ToString("N")
$runDir = Join-Path $PSScriptRoot "..\..\..\Saved\TextGen\IntegrationTests\SlotPerformance-$runId"
$slotDir = Join-Path $runDir "slots"
$presetPath = Join-Path $runDir "models.ini"
[System.IO.Directory]::CreateDirectory($slotDir) | Out-Null

$modelId = [System.IO.Path]::GetFileNameWithoutExtension($ModelPath) -replace '[\/:*?"<>|\[\]\s]', '_'
$preset = @(
    "version = 1"
    ""
    "[$modelId]"
    "model = $ModelPath"
    "ctx-size = 4096"
    "threads = $Threads"
    "batch-size = $BatchSize"
    "ubatch-size = $UBatchSize"
    "load-mode = mmap"
    "flash-attn = $FlashAttention"
    "cache-type-k = f16"
    "cache-type-v = f16"
    "reasoning = off"
    "reasoning-budget = 0"
    $(if ($SwaFull) { "swa-full = true" })
    "parallel = 1"
    $(if (-not $ContinuousBatching) { "no-cont-batching = true" })
    $(if ($CacheRamMiB -ge 0) { "cache-ram = $CacheRamMiB" })
    "poll = $Poll"
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

function Invoke-JsonPayload {
    param([string]$Uri, [string]$Method = "Get", [object]$Body)
    return (Invoke-JsonRequest -Uri $Uri -Method $Method -Body $Body).Content | ConvertFrom-Json
}

function Wait-ModelStatus {
    param([string]$BaseUrl, [string]$Expected, [datetime]$Deadline)
    do {
        $payload = Invoke-JsonPayload -Uri "$BaseUrl/models"
        $entry = @($payload.data) | Where-Object { $_.id -eq $modelId } | Select-Object -First 1
        $status = if ($null -eq $entry) { "unloaded" } else { [string]$entry.status.value }
        if ($status -eq $Expected) { return }
        if ($null -ne $entry -and $entry.status.failed) { throw "Model entered failed state: $($entry.status.error)" }
        Start-Sleep -Milliseconds 250
    } while ([DateTime]::UtcNow -lt $Deadline)
    throw "Model did not reach '$Expected' before timeout."
}

function Get-Timings {
    param([object]$Payload)
    if ($null -ne $Payload.timings) { return $Payload.timings }
    if ($null -ne $Payload.__verbose -and $null -ne $Payload.__verbose.timings) { return $Payload.__verbose.timings }
    throw "llama.cpp response did not contain timings."
}

function Get-LlamaProcessSnapshot {
    param([datetime]$StartedAfter)
    $processes = @(Get-Process -Name "llama-server" -ErrorAction SilentlyContinue | Where-Object {
        $_.StartTime -ge $StartedAfter
    })
    return [ordered]@{
        ProcessCount = $processes.Count
        CpuSeconds = [double](($processes | Measure-Object -Property CPU -Sum).Sum)
        WorkingSetBytes = [int64](($processes | Measure-Object -Property WorkingSet64 -Sum).Sum)
        PrivateBytes = [int64](($processes | Measure-Object -Property PrivateMemorySize64 -Sum).Sum)
    }
}

function Invoke-MeasuredCompletion {
    param([object]$Prompt, [int]$PredictTokens = 0)
    $timer = [System.Diagnostics.Stopwatch]::StartNew()
    $payload = Invoke-JsonPayload -Uri "$baseUrl/completion" -Method Post -Body @{
        model = $modelId
        prompt = $Prompt
        n_predict = $PredictTokens
        cache_prompt = $true
        id_slot = 0
        return_tokens = $true
    }
    $timer.Stop()
    $timings = Get-Timings -Payload $payload
    return [ordered]@{
        CacheTokens = [int]$timings.cache_n
        ProcessedTokens = [int]$timings.prompt_n
        PromptMilliseconds = [double]$timings.prompt_ms
        PredictedTokens = [int]$timings.predicted_n
        PredictedMilliseconds = [double]$timings.predicted_ms
        PredictedTokensPerSecond = [double]$timings.predicted_per_second
        RequestMilliseconds = [double]$timer.Elapsed.TotalMilliseconds
        Content = [string]$payload.content
        GeneratedTokenIds = @($payload.tokens)
    }
}

$startInfo = [System.Diagnostics.ProcessStartInfo]::new()
$startInfo.FileName = $server
$startInfo.WorkingDirectory = Split-Path -LiteralPath $server
$startInfo.UseShellExecute = $false
$startInfo.CreateNoWindow = $true
$startInfo.RedirectStandardOutput = $true
$startInfo.RedirectStandardError = $true
$serverArguments = if ($DirectServer) {
    $arguments = @("--host", "127.0.0.1", "--port", "$port", "--model", $ModelPath, "--alias", $modelId,
      "--ctx-size", "4096", "--threads", "$Threads", "--batch-size", "$BatchSize", "--ubatch-size", "$UBatchSize",
      "--flash-attn", $FlashAttention, "--parallel", "1", "--cache-type-k", "f16", "--cache-type-v", "f16", "--poll", "$Poll",
      "--reasoning", "off", "--slots", "--slot-save-path", $slotDir, "--no-webui", "--offline")
    if (-not $ContinuousBatching) { $arguments += "--no-cont-batching" }
    if ($CacheRamMiB -ge 0) { $arguments += @("--cache-ram", "$CacheRamMiB") }
    if ($SwaFull) { $arguments += "--swa-full" }
    $arguments
} else {
    @("--host", "127.0.0.1", "--port", "$port", "--models-preset", $presetPath, "--models-max", "1",
      "--no-models-autoload", "--slots", "--slot-save-path", $slotDir, "--no-webui", "--offline")
}
$serverArguments | ForEach-Object { [void]$startInfo.ArgumentList.Add($_) }

$process = [System.Diagnostics.Process]::new()
$process.StartInfo = $startInfo
if (-not $process.Start()) { throw "Failed to launch test llama.cpp router." }
$stdoutTask = $process.StandardOutput.ReadToEndAsync()
$stderrTask = $process.StandardError.ReadToEndAsync()
$baseUrl = "http://127.0.0.1:$port"
$result = $null

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

    if (-not $DirectServer) {
        Invoke-JsonRequest -Uri "$baseUrl/models?reload=1" | Out-Null
        Invoke-JsonRequest -Uri "$baseUrl/models/load" -Method Post -Body @{ model = $modelId } | Out-Null
        Wait-ModelStatus -BaseUrl $baseUrl -Expected "loaded" -Deadline $deadline
    }

    Start-Sleep -Seconds 1
    $idleSampleStart = Get-LlamaProcessSnapshot -StartedAfter $process.StartTime
    $idleTimer = [System.Diagnostics.Stopwatch]::StartNew()
    Start-Sleep -Seconds 3
    $idleTimer.Stop()
    $idleSampleEnd = Get-LlamaProcessSnapshot -StartedAfter $process.StartTime

    $commonFacts = 1..$CommonPromptRepeats | ForEach-Object {
        "Persistent fact $_`: the archival marker is value $($_ * 17), and it must remain available throughout this conversation."
    }
    $systemPrompt = "You are a deterministic slot-cache performance test assistant. " + ($commonFacts -join " ")
    $userPrompt = "Acknowledge that the persistent archival facts are available using one short sentence."

    $baseTemplate = Invoke-JsonPayload -Uri "$baseUrl/apply-template" -Method Post -Body @{
        model = $modelId
        add_generation_prompt = $false
        messages = @(@{ role = "system"; content = $systemPrompt })
    }
    $basePrompt = [string]$baseTemplate.prompt
    $fullTemplate = Invoke-JsonPayload -Uri "$baseUrl/apply-template" -Method Post -Body @{
        model = $modelId
        add_generation_prompt = $true
        messages = @(
            @{ role = "system"; content = $systemPrompt }
            @{ role = "user"; content = $userPrompt }
        )
    }
    $fullPrompt = [string]$fullTemplate.prompt
    if ([string]::IsNullOrWhiteSpace($basePrompt) -or [string]::IsNullOrWhiteSpace($fullPrompt)) {
        throw "apply-template returned an empty prompt."
    }
    if (-not $fullPrompt.StartsWith($basePrompt, [System.StringComparison]::Ordinal)) {
        throw "The generated default-cache prompt is not a prefix of the runtime prompt."
    }

    $baseTokens = @((Invoke-JsonPayload -Uri "$baseUrl/tokenize" -Method Post -Body @{
        model = $modelId
        content = $basePrompt
        add_special = $true
        parse_special = $true
    }).tokens)
    $fullTokens = @((Invoke-JsonPayload -Uri "$baseUrl/tokenize" -Method Post -Body @{
        model = $modelId
        content = $fullPrompt
        add_special = $true
        parse_special = $true
    }).tokens)
    $commonTokenCount = 0
    while ($commonTokenCount -lt $baseTokens.Count -and
        $commonTokenCount -lt $fullTokens.Count -and
        [int64]$baseTokens[$commonTokenCount] -eq [int64]$fullTokens[$commonTokenCount]) {
        $commonTokenCount++
    }
    $tokenPrefixRatio = if ($baseTokens.Count -gt 0) { $commonTokenCount / $baseTokens.Count } else { 0.0 }
    if ($tokenPrefixRatio -lt 0.99) {
        throw "The default-cache token sequence is not a stable runtime prefix ($commonTokenCount/$($baseTokens.Count) tokens)."
    }

    $completionBasePrompt = if ($UseTokenPrompts) { $baseTokens } else { $basePrompt }
    $completionFullPrompt = if ($UseTokenPrompts) { $fullTokens } else { $fullPrompt }

    $slotBody = @{ model = $modelId; filename = "SlotPerformance.bin" }
    Invoke-JsonRequest -Uri "$baseUrl/slots/0?action=erase" -Method Post -Body @{ model = $modelId } | Out-Null
    $cold = Invoke-MeasuredCompletion -Prompt $completionFullPrompt
    $immediateRepeat = Invoke-MeasuredCompletion -Prompt $completionFullPrompt

    Invoke-JsonRequest -Uri "$baseUrl/slots/0?action=erase" -Method Post -Body @{ model = $modelId } | Out-Null
    $prefill = Invoke-MeasuredCompletion -Prompt $completionBasePrompt
    $save = Invoke-JsonPayload -Uri "$baseUrl/slots/0?action=save" -Method Post -Body $slotBody
    $checkpointPath = Join-Path $slotDir $slotBody.filename
    if (-not (Test-Path -LiteralPath $checkpointPath -PathType Leaf)) {
        throw "Slot save did not create $checkpointPath."
    }

    Invoke-JsonRequest -Uri "$baseUrl/slots/0?action=erase" -Method Post -Body @{ model = $modelId } | Out-Null
    $restore = Invoke-JsonPayload -Uri "$baseUrl/slots/0?action=restore" -Method Post -Body $slotBody
    $warm = Invoke-MeasuredCompletion -Prompt $completionFullPrompt

    Invoke-JsonRequest -Uri "$baseUrl/slots/0?action=erase" -Method Post -Body @{ model = $modelId } | Out-Null
    $firstTurn = Invoke-MeasuredCompletion -Prompt $completionFullPrompt -PredictTokens 48
    if ([string]::IsNullOrWhiteSpace($firstTurn.Content) -or $firstTurn.GeneratedTokenIds.Count -eq 0) {
        throw "The multi-turn cache test did not receive an assistant response."
    }

    $secondUserPrompt = "Confirm once more that the archival facts remain available."
    $secondTurnTemplate = Invoke-JsonPayload -Uri "$baseUrl/apply-template" -Method Post -Body @{
        model = $modelId
        add_generation_prompt = $true
        messages = @(
            @{ role = "system"; content = $systemPrompt }
            @{ role = "user"; content = $userPrompt }
            @{ role = "assistant"; content = $firstTurn.Content }
            @{ role = "user"; content = $secondUserPrompt }
        )
    }
    $secondTurnPrompt = [string]$secondTurnTemplate.prompt
    $secondTurnTokens = @((Invoke-JsonPayload -Uri "$baseUrl/tokenize" -Method Post -Body @{
        model = $modelId
        content = $secondTurnPrompt
        add_special = $true
        parse_special = $true
    }).tokens)
    $secondTurnCompletionPrompt = if ($UseTokenPrompts) { $secondTurnTokens } else { $secondTurnPrompt }
    $secondTurn = Invoke-MeasuredCompletion -Prompt $secondTurnCompletionPrompt
    $cachedAssistantTokens = [Math]::Max(0, $secondTurn.CacheTokens - $fullTokens.Count)
    $assistantHistoryCacheVerified = $cachedAssistantTokens -gt 0 -and
        $secondTurn.CacheTokens -gt $fullTokens.Count -and
        $secondTurn.ProcessedTokens -lt $secondTurnTokens.Count
    $finalProcessSnapshot = Get-LlamaProcessSnapshot -StartedAfter $process.StartTime

    $totalWarmPromptTokens = $warm.CacheTokens + $warm.ProcessedTokens
    $cacheHitRatio = if ($totalWarmPromptTokens -gt 0) { $warm.CacheTokens / $totalWarmPromptTokens } else { 0.0 }
    $warmPromptRatio = if ($cold.ProcessedTokens -gt 0) { $warm.ProcessedTokens / $cold.ProcessedTokens } else { 1.0 }
    $warmTimeRatio = if ($cold.PromptMilliseconds -gt 0.0) { $warm.PromptMilliseconds / $cold.PromptMilliseconds } else { 1.0 }

    $result = [ordered]@{
        Passed = $cacheHitRatio -ge $MinimumCacheHitRatio -and
            $warmPromptRatio -le $MaximumWarmPromptRatio -and
            $warmTimeRatio -le $MaximumWarmTimeRatio -and
            $assistantHistoryCacheVerified
        RuntimeTag = $Tag
        Model = $modelId
        ServerMode = if ($DirectServer) { "Direct" } else { "Router" }
        PromptMode = if ($UseTokenPrompts) { "TokenIds" } else { "String" }
        SwaFull = [bool]$SwaFull
        Threads = $Threads
        BatchSize = $BatchSize
        UBatchSize = $UBatchSize
        FlashAttention = $FlashAttention
        ContinuousBatching = [bool]$ContinuousBatching
        CacheRamMiB = $CacheRamMiB
        Poll = $Poll
        Idle = [ordered]@{
            WallMilliseconds = [double]$idleTimer.Elapsed.TotalMilliseconds
            CpuMilliseconds = [Math]::Max(0.0, 1000.0 * ($idleSampleEnd.CpuSeconds - $idleSampleStart.CpuSeconds))
            ProcessCount = $idleSampleEnd.ProcessCount
        }
        ProcessesAfterWorkload = $finalProcessSnapshot
        CheckpointBytes = (Get-Item -LiteralPath $checkpointPath).Length
        SavedTokens = [int]$save.n_saved
        RestoredTokens = [int]$restore.n_restored
        RestoreMilliseconds = [double]$restore.timings.restore_ms
        BasePromptTokens = $baseTokens.Count
        FullPromptTokens = $fullTokens.Count
        CommonPrefixTokens = $commonTokenCount
        TokenPrefixRatio = $tokenPrefixRatio
        Cold = $cold
        ImmediateRepeat = $immediateRepeat
        Prefill = $prefill
        Warm = $warm
        MultiTurn = [ordered]@{
            FirstResponseGeneratedTokens = $firstTurn.GeneratedTokenIds.Count
            SecondTurnPromptTokens = $secondTurnTokens.Count
            SecondTurnCachedTokens = $secondTurn.CacheTokens
            SecondTurnProcessedTokens = $secondTurn.ProcessedTokens
            CachedAssistantTokens = $cachedAssistantTokens
            AssistantHistoryCacheVerified = $assistantHistoryCacheVerified
        }
        CacheHitRatio = $cacheHitRatio
        WarmPromptRatio = $warmPromptRatio
        WarmTimeRatio = $warmTimeRatio
        Thresholds = [ordered]@{
            MinimumCacheHitRatio = $MinimumCacheHitRatio
            MaximumWarmPromptRatio = $MaximumWarmPromptRatio
            MaximumWarmTimeRatio = $MaximumWarmTimeRatio
        }
    }
    $result | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $runDir "result.json") -Encoding utf8NoBOM

    Write-Host ("Cold: {0} processed, {1} cached, {2:N1} ms" -f $cold.ProcessedTokens, $cold.CacheTokens, $cold.PromptMilliseconds)
    Write-Host ("Repeat: {0} processed, {1} cached, {2:N1} ms" -f $immediateRepeat.ProcessedTokens, $immediateRepeat.CacheTokens, $immediateRepeat.PromptMilliseconds)
    Write-Host ("Warm: {0} processed, {1} cached, {2:N1} ms" -f $warm.ProcessedTokens, $warm.CacheTokens, $warm.PromptMilliseconds)
    Write-Host ("Restore: {0} tokens in {1:N1} ms; cache hit {2:P1}; prompt-time reduction {3:P1}" -f
        $result.RestoredTokens, $result.RestoreMilliseconds, $cacheHitRatio, (1.0 - $warmTimeRatio))
    Write-Host ("Multi-turn: {0} assistant tokens generated; {1} assistant/history tokens cached; second turn {2} cached, {3} processed" -f
        $firstTurn.GeneratedTokenIds.Count, $cachedAssistantTokens, $secondTurn.CacheTokens, $secondTurn.ProcessedTokens)
    Write-Host "Result: $(Join-Path $runDir 'result.json')"

    if (-not $result.Passed) {
        throw "Slot checkpoint did not satisfy persistent or multi-turn cache requirements."
    }
    Write-Host "PASS: restored .bin reduced prompt tokens and prompt evaluation time." -ForegroundColor Green
} finally {
    if (-not $process.HasExited) {
        $process.Kill($true)
        $process.WaitForExit(10000) | Out-Null
    }
    [System.IO.File]::WriteAllText((Join-Path $runDir "stdout.log"), $stdoutTask.GetAwaiter().GetResult())
    [System.IO.File]::WriteAllText((Join-Path $runDir "stderr.log"), $stderrTask.GetAwaiter().GetResult())
    $process.Dispose()
}
