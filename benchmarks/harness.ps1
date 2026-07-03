# shm benchmark harness — sweeps a configuration matrix and emits CSV + Markdown.
#
# Usage:
#   pwsh ./benchmarks/harness.ps1                           # quick matrix (~2 min)
#   pwsh ./benchmarks/harness.ps1 -Profile full             # full matrix (~10 min)
#   pwsh ./benchmarks/harness.ps1 -Profile stream           # stream-mode sweep
#   pwsh ./benchmarks/harness.ps1 -Threads 1,4,8 -Payloads 64,1024 -Duration 5
#   pwsh ./benchmarks/harness.ps1 -SkipBuild                # reuse existing binaries
#   pwsh ./benchmarks/harness.ps1 -OutDir results/2026-05-16
#
# Output:
#   <OutDir>/results.csv     — one row per run (parseable)
#   <OutDir>/summary.md      — human-readable summary with peak rows highlighted
#   <OutDir>/raw/*.log       — captured stdout/stderr for every run
#
# Notes:
#   * Run from the shm/ repository root (one level above benchmarks/).
#   * Matches Direct Exchange invariants from SPECIFICATION.md: numHostSlots == NUM_THREADS,
#     1:1 thread-to-slot mapping, so "Threads" is the only knob that varies slot count.
#   * WaitStrategy params are currently hard-coded in WaitStrategy.h / spin.go — vary them
#     by recompiling with different defaults and re-running with -SkipBuild=$false.

[CmdletBinding()]
param(
    [ValidateSet('quick', 'full', 'stream', 'custom')]
    [string]$Profile = 'quick',

    # Accepts a comma list ("1,4,8") or an array — pwsh -File collapses arrays into
    # a single string, so we normalise after parsing.
    [object]$Threads,
    [object]$Payloads,
    [int]$Duration = 10,
    [int]$Repeats = 1,
    [ValidateSet('normal', 'stream', 'guest-call')]
    [string]$Mode = 'normal',
    [int]$ChunkSize = 4096,
    # In-flight chunks per stream worker (StreamSender pipelining depth).
    # 0 = let main.cpp pick the mode default (stream=4, normal=1). Set to 1
    # to reproduce the pre-v0.7.12 serialized-chunk numbers.
    [int]$InFlight = 0,
    # Worker CPU affinity mode.
    #   'auto'    (default) — pin slot-N's host worker thread and Go guest
    #                         goroutine to CCX[N % numCCX] WHEN the host
    #                         reports >1 shared-L3 group (chiplet AMD,
    #                         multi-socket Xeon). No-op on monolithic-L3
    #                         hosts (most single-socket Intel desktops).
    #   'none'              — explicit OS-scheduler-decides.
    #   'local'             — force CCX pin even on monolithic-L3.
    #   'sibling'           — opt-in: pin host worker N to one SMT LP of
    #                         physical core [N % numPairs], guest to the
    #                         other LP. Trades pipeline resource sharing
    #                         for shared-L1d coherency on the SlotHeader
    #                         line. Falls back to no-pin if the host has
    #                         no LTP_PC_SMT pairs (E-only / no-SMT / VM).
    [ValidateSet('auto','none','local','sibling')]
    [string]$Affinity = 'auto',

    [string]$OutDir,
    [string]$ShmName = 'SimpleIPC',

    [switch]$SkipBuild,
    [switch]$VerboseRun,
    # Normal mode: use the pre-v0.8.5 per-op claim/free cycle instead of the
    # held-slot session (passes --legacy-claim to the C++ client). For A/B
    # isolation of the held-slot send path.
    [switch]$LegacyClaim,
    [int]$StartupSeconds = 2,
    [int]$TimeoutSeconds = 90,

    # Pin C++ client and Go server to PROCESS_PRIORITY_CLASS=HIGH and switch the
    # Windows power plan to "Ultimate Performance" (or High Performance fallback)
    # for the duration of the run. This is the single biggest knob for IPC
    # latency on laptops with aggressive C-state parking. Restored on exit.
    [switch]$HighPriority
)

$ErrorActionPreference = 'Stop'
$script:RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path

# ----------------------------------------------------------------------------
# Profile expansion
# ----------------------------------------------------------------------------

function Resolve-Matrix {
    param([string]$Profile)

    switch ($Profile) {
        'quick' {
            return @{
                Threads  = @(1, 4, 8)
                Payloads = @(64, 1024)
                Mode     = 'normal'
            }
        }
        'full' {
            return @{
                Threads  = @(1, 2, 4, 8, 12, 16, 24)
                Payloads = @(64, 256, 1024, 4096, 16384)
                Mode     = 'normal'
            }
        }
        'stream' {
            return @{
                Threads  = @(1, 4, 8)
                Payloads = @(65536, 1048576, 16777216)
                Mode     = 'stream'
            }
        }
        default {
            return @{}
        }
    }
}

function ConvertTo-IntArray {
    param($Value)
    if ($null -eq $Value) { return $null }
    if ($Value -is [int[]]) { return $Value }
    if ($Value -is [array]) {
        return @($Value | ForEach-Object { [int]$_ })
    }
    # Single string like "1,4,8" — split on commas/whitespace.
    return @(([string]$Value) -split '[,\s]+' | Where-Object { $_ } | ForEach-Object { [int]$_ })
}

$Threads  = ConvertTo-IntArray $Threads
$Payloads = ConvertTo-IntArray $Payloads

if ($Profile -ne 'custom') {
    $defaults = Resolve-Matrix -Profile $Profile
    if (-not $Threads)  { $Threads  = $defaults.Threads }
    if (-not $Payloads) { $Payloads = $defaults.Payloads }
    if (-not $PSBoundParameters.ContainsKey('Mode')) { $Mode = $defaults.Mode }
}

if (-not $Threads -or -not $Payloads) {
    throw "Threads and Payloads must be specified (either via -Profile or explicit lists)."
}

# ----------------------------------------------------------------------------
# Paths
# ----------------------------------------------------------------------------

if (-not $OutDir) {
    $stamp = Get-Date -Format 'yyyyMMdd_HHmmss'
    $OutDir = Join-Path $RepoRoot "benchmarks/results/$stamp"
}
$OutDir = [System.IO.Path]::GetFullPath($OutDir)
$RawDir = Join-Path $OutDir 'raw'
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
New-Item -ItemType Directory -Force -Path $RawDir | Out-Null

$BuildDir         = Join-Path $RepoRoot 'build_bench'
$CppBinary        = Join-Path $BuildDir 'benchmarks/Release/shm_benchmark.exe'
$CppBinaryFallback = Join-Path $BuildDir 'benchmarks/shm_benchmark.exe'
$GoBinary         = Join-Path $RepoRoot 'benchmarks/go/server.exe'

# ----------------------------------------------------------------------------
# Build
# ----------------------------------------------------------------------------

function Invoke-Build {
    Write-Host "[Build] C++ host..." -ForegroundColor Cyan
    New-Item -ItemType Directory -Force -Path $BuildDir | Out-Null
    & cmake -S $RepoRoot -B $BuildDir -DSHM_BUILD_BENCHMARKS=ON -DCMAKE_BUILD_TYPE=Release
    if ($LASTEXITCODE -ne 0) { throw "cmake configure failed (exit $LASTEXITCODE)" }
    & cmake --build $BuildDir --config Release
    if ($LASTEXITCODE -ne 0) { throw "cmake build failed (exit $LASTEXITCODE)" }

    Write-Host "[Build] Go guest..." -ForegroundColor Cyan
    Push-Location (Join-Path $RepoRoot 'benchmarks/go')
    try {
        & go build -tags shm_benchstats -o server.exe main.go
        if ($LASTEXITCODE -ne 0) { throw "go build failed (exit $LASTEXITCODE)" }
    } finally {
        Pop-Location
    }
}

function Resolve-CppBinary {
    if (Test-Path $CppBinary)          { return $CppBinary }
    if (Test-Path $CppBinaryFallback)  { return $CppBinaryFallback }
    throw "C++ benchmark binary not found. Expected at: $CppBinary or $CppBinaryFallback"
}

if (-not $SkipBuild) {
    Invoke-Build
}
$CppBinaryResolved = Resolve-CppBinary
if (-not (Test-Path $GoBinary)) {
    throw "Go benchmark binary not found at $GoBinary. Re-run without -SkipBuild."
}

# ----------------------------------------------------------------------------
# Cleanup helpers
# ----------------------------------------------------------------------------

function Stop-Stragglers {
    foreach ($name in @('shm_benchmark', 'server')) {
        try {
            Get-Process -Name $name -ErrorAction Stop |
                Stop-Process -Force -ErrorAction SilentlyContinue
        } catch { }
    }
}

function Stop-Tree {
    param([int]$ProcessId)
    if (-not $ProcessId) { return }
    try {
        $children = Get-CimInstance Win32_Process -Filter "ParentProcessId=$ProcessId" -ErrorAction SilentlyContinue
        foreach ($c in $children) { Stop-Tree -ProcessId $c.ProcessId }
        Stop-Process -Id $ProcessId -Force -ErrorAction SilentlyContinue
    } catch { }
}

function Set-ProcessPriorityHigh {
    param([int]$ProcessId)
    if (-not $ProcessId) { return }
    try {
        $p = Get-Process -Id $ProcessId -ErrorAction Stop
        $p.PriorityClass = [System.Diagnostics.ProcessPriorityClass]::High
    } catch { }
}

# Windows built-in GUID for "Ultimate Performance"; falls back to "High Performance".
$script:UltimatePerfGuid    = 'e9a42b02-d5df-448d-aa00-03f14749eb61'
$script:HighPerfGuid        = '8c5e7fda-e8bf-4a96-9a85-a6e23a8c635c'
$script:OriginalPowerScheme = $null

function Push-PowerPlan {
    # Returns the previously-active scheme GUID so we can restore it.
    $active = (& powercfg /getactivescheme) 2>$null
    if ($active -match '([0-9a-fA-F-]{36})') { $script:OriginalPowerScheme = $Matches[1] }

    # Try Ultimate first; if it doesn't exist, enable it (Win10+ supports this).
    & powercfg /setactive $script:UltimatePerfGuid 2>$null | Out-Null
    $active2 = (& powercfg /getactivescheme) 2>$null
    if ($active2 -notmatch [regex]::Escape($script:UltimatePerfGuid)) {
        & powercfg -duplicatescheme $script:UltimatePerfGuid 2>$null | Out-Null
        & powercfg /setactive $script:UltimatePerfGuid 2>$null | Out-Null
        $active2 = (& powercfg /getactivescheme) 2>$null
    }
    if ($active2 -notmatch [regex]::Escape($script:UltimatePerfGuid)) {
        & powercfg /setactive $script:HighPerfGuid 2>$null | Out-Null
        $active2 = (& powercfg /getactivescheme) 2>$null
    }
    Write-Host "[Harness] Power plan set to: $active2"
}

function Pop-PowerPlan {
    if ($script:OriginalPowerScheme) {
        & powercfg /setactive $script:OriginalPowerScheme 2>$null | Out-Null
        Write-Host "[Harness] Power plan restored to: $script:OriginalPowerScheme"
        $script:OriginalPowerScheme = $null
    }
}

# ----------------------------------------------------------------------------
# Output parser — pulls numeric results from the C++ client stdout.
# Lines look like:
#   Total Ops:      29,225,460
#   Throughput:     2,922,546.00 ops/s
#   Avg Latency:    0.67 us
#   Errors:         0
# ----------------------------------------------------------------------------

function Parse-CppOutput {
    param([string]$Text)

    $out = [ordered]@{
        TotalOps    = $null
        Throughput  = $null
        AvgLatency  = $null
        Errors      = $null
        Bandwidth   = $null
    }

    $clean = { param($v) ($v -replace ',', '').Trim() }

    if ($Text -match 'Total Ops:\s+([\d,]+)')              { $out.TotalOps   = [int64] (& $clean $Matches[1]) }
    if ($Text -match 'Throughput:\s+([\d,\.]+)\s+ops/s')   { $out.Throughput = [double](& $clean $Matches[1]) }
    if ($Text -match 'Avg Latency:\s+([\d\.]+)\s+us')      { $out.AvgLatency = [double]$Matches[1] }
    if ($Text -match 'Errors:\s+([\d,]+)')                 { $out.Errors     = [int64] (& $clean $Matches[1]) }
    if ($Text -match 'Bandwidth:\s+([\d,\.]+)\s+MB/s')     { $out.Bandwidth  = [double](& $clean $Matches[1]) }

    return $out
}

# ----------------------------------------------------------------------------
# Single-run driver
# ----------------------------------------------------------------------------

function Invoke-Case {
    param(
        [int]$ThreadCount,
        [int]$PayloadSize,
        [int]$Iteration
    )

    $tag = "t${ThreadCount}_p${PayloadSize}_i${Iteration}_${Mode}"
    $serverStdout = Join-Path $RawDir "$tag.server.stdout.log"
    $serverStderr = Join-Path $RawDir "$tag.server.stderr.log"
    $clientStdout = Join-Path $RawDir "$tag.client.stdout.log"
    $clientStderr = Join-Path $RawDir "$tag.client.stderr.log"

    Stop-Stragglers

    # Start Go server
    $serverArgs = @('-w', $ThreadCount, '-name', $ShmName, '-affinity', $Affinity)
    if ($Mode -eq 'stream')     { $serverArgs += '-stream' }
    if ($Mode -eq 'guest-call') { $serverArgs += '-guest-call' }
    if ($VerboseRun)            { $serverArgs += '-v' }

    $server = Start-Process -FilePath $GoBinary `
        -ArgumentList $serverArgs `
        -RedirectStandardOutput $serverStdout `
        -RedirectStandardError $serverStderr `
        -PassThru -WindowStyle Hidden

    if ($HighPriority) { Set-ProcessPriorityHigh -ProcessId $server.Id }

    Start-Sleep -Seconds $StartupSeconds

    # Start C++ client
    $clientArgs = @('-t', $ThreadCount, '-s', $PayloadSize, '-d', $Duration, '--name', $ShmName, '--affinity', $Affinity)
    if ($Mode -eq 'stream')     { $clientArgs += @('--stream', '-c', $ChunkSize) }
    if ($Mode -eq 'guest-call') { $clientArgs += '--guest-call' }
    if ($InFlight -gt 0)        { $clientArgs += @('-i', $InFlight) }
    if ($LegacyClaim)           { $clientArgs += '--legacy-claim' }
    if ($VerboseRun)            { $clientArgs += '-v' }

    $client = Start-Process -FilePath $CppBinaryResolved `
        -ArgumentList $clientArgs `
        -RedirectStandardOutput $clientStdout `
        -RedirectStandardError $clientStderr `
        -PassThru -WindowStyle Hidden

    if ($HighPriority) { Set-ProcessPriorityHigh -ProcessId $client.Id }

    $exited = $client.WaitForExit(($TimeoutSeconds * 1000))
    $timedOut = -not $exited
    if ($timedOut) {
        Stop-Tree -ProcessId $client.Id
    }
    $exitCode = if ($timedOut) { -1 } else { $client.ExitCode }

    # Stop server
    Stop-Tree -ProcessId $server.Id

    $clientText = if (Test-Path $clientStdout) { Get-Content $clientStdout -Raw } else { '' }
    $parsed = Parse-CppOutput -Text $clientText

    $row = [pscustomobject]@{
        Mode        = $Mode
        Threads     = $ThreadCount
        Payload     = $PayloadSize
        Iteration   = $Iteration
        Duration    = $Duration
        TotalOps    = $parsed.TotalOps
        Throughput  = $parsed.Throughput
        AvgLatency  = $parsed.AvgLatency
        Errors      = $parsed.Errors
        Bandwidth   = $parsed.Bandwidth
        TimedOut    = $timedOut
        ExitCode    = $exitCode
        LogTag      = $tag
    }
    return $row
}

# ----------------------------------------------------------------------------
# Matrix execution
# ----------------------------------------------------------------------------

$rows = New-Object System.Collections.Generic.List[object]
$total = $Threads.Count * $Payloads.Count * $Repeats
$caseNo = 0
$startedAt = Get-Date

Write-Host ""
Write-Host "[Harness] Repo:    $RepoRoot"
Write-Host "[Harness] OutDir:  $OutDir"
$inFlightLabel = if ($InFlight -gt 0) { "inFlight=$InFlight" } else { "inFlight=mode-default" }
Write-Host "[Harness] Matrix:  threads=$($Threads -join ',') payloads=$($Payloads -join ',') mode=$Mode duration=${Duration}s repeats=$Repeats $inFlightLabel affinity=$Affinity (total=$total)"
if ($HighPriority) { Write-Host "[Harness] HighPriority: ON (process priority HIGH, Ultimate/High power plan)" }
Write-Host ""

if ($HighPriority) { Push-PowerPlan }
try {

foreach ($p in $Payloads) {
    foreach ($t in $Threads) {
        for ($i = 1; $i -le $Repeats; $i++) {
            $caseNo++
            Write-Host ("[{0}/{1}] threads={2,-3} payload={3,-7} iter={4}" -f $caseNo, $total, $t, $p, $i) -ForegroundColor Yellow
            try {
                $row = Invoke-Case -ThreadCount $t -PayloadSize $p -Iteration $i
                $rows.Add($row)
                $thr = if ($row.Throughput) { '{0:N0}' -f $row.Throughput } else { '?' }
                $lat = if ($row.AvgLatency) { '{0:N2}us' -f $row.AvgLatency } else { '?' }
                $err = if ($row.Errors)     { $row.Errors } else { 0 }
                $status = if ($row.TimedOut) { 'TIMEOUT' } elseif ($row.ExitCode -ne 0) { "EXIT=$($row.ExitCode)" } else { 'ok' }
                Write-Host ("        -> {0} ops/s, lat {1}, errors {2}, status {3}" -f $thr, $lat, $err, $status)
            } catch {
                Write-Host ("        -> FAILED: {0}" -f $_.Exception.Message) -ForegroundColor Red
                $rows.Add([pscustomobject]@{
                    Mode = $Mode; Threads = $t; Payload = $p; Iteration = $i; Duration = $Duration;
                    TotalOps = $null; Throughput = $null; AvgLatency = $null; Errors = $null; Bandwidth = $null;
                    TimedOut = $false; ExitCode = -2; LogTag = "t${t}_p${p}_i${i}_${Mode}";
                })
            }
        }
    }
}
Stop-Stragglers
} finally {
    if ($HighPriority) { Pop-PowerPlan }
}

$elapsed = (Get-Date) - $startedAt

# ----------------------------------------------------------------------------
# CSV
# ----------------------------------------------------------------------------

$csvPath = Join-Path $OutDir 'results.csv'
$rows | Export-Csv -Path $csvPath -NoTypeInformation -Encoding UTF8
Write-Host ""
Write-Host "[Harness] Wrote $csvPath" -ForegroundColor Green

# ----------------------------------------------------------------------------
# Markdown summary
# ----------------------------------------------------------------------------

function Format-Throughput {
    param($V)
    if ($null -eq $V) { return '-' }
    return ('{0:N0}' -f $V)
}
function Format-Latency {
    param($V)
    if ($null -eq $V) { return '-' }
    return ('{0:N2}' -f $V)
}

$md = New-Object System.Text.StringBuilder
[void]$md.AppendLine("# shm Benchmark Run")
[void]$md.AppendLine()
[void]$md.AppendLine("- **Started:** $($startedAt.ToString('yyyy-MM-dd HH:mm:ss'))")
[void]$md.AppendLine("- **Elapsed:** $([int]$elapsed.TotalSeconds)s")
[void]$md.AppendLine("- **Host:** $env:COMPUTERNAME ($env:PROCESSOR_IDENTIFIER, $env:NUMBER_OF_PROCESSORS LP)")
[void]$md.AppendLine("- **OS:** $([System.Environment]::OSVersion.VersionString)")
[void]$md.AppendLine("- **Mode:** $Mode")
[void]$md.AppendLine("- **Duration:** ${Duration}s per case, $Repeats repeat(s)")
[void]$md.AppendLine("- **Threads:** $($Threads -join ', ')")
[void]$md.AppendLine("- **Payloads:** $($Payloads -join ', ') bytes")
[void]$md.AppendLine()

# Best-of-Repeats aggregation
$agg = $rows | Group-Object Mode, Threads, Payload | ForEach-Object {
    $best = $_.Group | Sort-Object Throughput -Descending | Select-Object -First 1
    $best
}

# Throughput pivot table: rows = payload, cols = threads
[void]$md.AppendLine("## Throughput (ops/s) — best of $Repeats")
[void]$md.AppendLine()
$header = "| Payload \\ Threads |" + (($Threads | ForEach-Object { " $_ |" }) -join '')
[void]$md.AppendLine($header)
[void]$md.AppendLine("|" + (("---|") * ($Threads.Count + 1)))
foreach ($p in $Payloads) {
    $line = "| $p |"
    foreach ($t in $Threads) {
        $cell = $agg | Where-Object { $_.Threads -eq $t -and $_.Payload -eq $p }
        $line += " $(Format-Throughput $cell.Throughput) |"
    }
    [void]$md.AppendLine($line)
}
[void]$md.AppendLine()

# Latency pivot table
[void]$md.AppendLine("## Avg Latency (us) — best-throughput run")
[void]$md.AppendLine()
[void]$md.AppendLine($header)
[void]$md.AppendLine("|" + (("---|") * ($Threads.Count + 1)))
foreach ($p in $Payloads) {
    $line = "| $p |"
    foreach ($t in $Threads) {
        $cell = $agg | Where-Object { $_.Threads -eq $t -and $_.Payload -eq $p }
        $line += " $(Format-Latency $cell.AvgLatency) |"
    }
    [void]$md.AppendLine($line)
}
[void]$md.AppendLine()

# Peak rows
$peakThroughput = $agg | Sort-Object Throughput -Descending | Select-Object -First 1
$peakLatency    = $agg | Where-Object { $_.AvgLatency } | Sort-Object AvgLatency | Select-Object -First 1
[void]$md.AppendLine("## Peaks")
[void]$md.AppendLine()
if ($peakThroughput) {
    [void]$md.AppendLine("- **Peak throughput:** $(Format-Throughput $peakThroughput.Throughput) ops/s @ threads=$($peakThroughput.Threads), payload=$($peakThroughput.Payload)")
}
if ($peakLatency) {
    [void]$md.AppendLine("- **Lowest latency:**  $(Format-Latency $peakLatency.AvgLatency) us @ threads=$($peakLatency.Threads), payload=$($peakLatency.Payload)")
}
[void]$md.AppendLine()

# Errors / timeouts
$bad = $rows | Where-Object { $_.TimedOut -or $_.ExitCode -ne 0 -or ($_.Errors -and $_.Errors -gt 0) }
if ($bad) {
    [void]$md.AppendLine("## Anomalies")
    [void]$md.AppendLine()
    [void]$md.AppendLine("| Threads | Payload | Iter | Errors | TimedOut | ExitCode | Log |")
    [void]$md.AppendLine("|---|---|---|---|---|---|---|")
    foreach ($b in $bad) {
        [void]$md.AppendLine("| $($b.Threads) | $($b.Payload) | $($b.Iteration) | $($b.Errors) | $($b.TimedOut) | $($b.ExitCode) | raw/$($b.LogTag).client.stdout.log |")
    }
    [void]$md.AppendLine()
}

$mdPath = Join-Path $OutDir 'summary.md'
[System.IO.File]::WriteAllText($mdPath, $md.ToString(), [System.Text.UTF8Encoding]::new($false))
Write-Host "[Harness] Wrote $mdPath" -ForegroundColor Green
Write-Host ""
Write-Host "[Harness] Done in $([int]$elapsed.TotalSeconds)s." -ForegroundColor Green
