# Stop on first error
$ErrorActionPreference = "Stop"

# Cleanup function
function Cleanup-Processes {
    Write-Host "Cleaning up old processes..."
    # Stop any running server or benchmark processes from previous runs
    Get-Process | Where-Object { $_.Name -in @("server", "shm_benchmark") } | Stop-Process -Force -ErrorAction SilentlyContinue
}

# Initial cleanup
Cleanup-Processes

# Build C++
Write-Host "[Build] C++ Host..."
# Create build directory if it doesn't exist
New-Item -Path "benchmarks/build" -ItemType Directory -Force | Out-Null
# Change to build directory
Set-Location -Path "benchmarks/build"

# Run CMake and build
# Note: This assumes a suitable generator like "Visual Studio" or "Ninja" is available.
# To be more robust, one could specify a generator with -G, but for now we'll let CMake decide.
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build . --config Release

# Change back to the project root
Set-Location -Path "../.."

# Build Go
Write-Host "[Build] Go Guest..."
Set-Location -Path "benchmarks/go"
go build -o server.exe main.go
Set-Location -Path "../.."

# Function to run a benchmark case
function Run-Case {
    param(
        [int]$Threads
    )

    Write-Host "----------------------------------------"
    Write-Host "Running Case: $Threads Threads"
    Write-Host "----------------------------------------"

    # Cleanup before run
    Cleanup-Processes

    # Start Go server in the background
    $stdoutLogFile = "server_${Threads}_stdout.log"
    $stderrLogFile = "server_${Threads}_stderr.log"
    $serverProcess = Start-Process -FilePath (Resolve-Path -Path "./benchmarks/go/server.exe").Path -ArgumentList "-w $Threads" -RedirectStandardOutput $stdoutLogFile -RedirectStandardError $stderrLogFile -PassThru
    Write-Host "Started Go server with PID: $($serverProcess.Id)"

    # Give the server a moment to start up
    Start-Sleep -Seconds 2

    # Run C++ client and wait for it to finish, with a timeout
    $benchmarkExe = (Resolve-Path -Path "./benchmarks/build/shm_benchmark.exe").Path
    $benchmarkArgs = "-t $Threads"
    if ($env:VERBOSE -eq "1") {
        $benchmarkArgs += " -v"
    }

    Write-Host "Starting C++ benchmark client..."
    $clientJob = Start-Job -ScriptBlock {
        param($Executable, $Arguments)
        $tempStdoutFile = "job_stdout_$(Get-Random).txt"
        $tempStderrFile = "job_stderr_$(Get-Random).txt"
        
        try {
            # We must use Start-Process and wait for it inside the job
            $process = Start-Process -FilePath $Executable -ArgumentList $Arguments -RedirectStandardOutput $tempStdoutFile -RedirectStandardError $tempStderrFile -Wait -PassThru
            $exitCode = $process.ExitCode
        } finally {
            $stdoutContent = Get-Content -Path $tempStdoutFile -ErrorAction SilentlyContinue | Out-String
            $stderrContent = Get-Content -Path $tempStderrFile -ErrorAction SilentlyContinue | Out-String
            Remove-Item -Path $tempStdoutFile, $tempStderrFile -ErrorAction SilentlyContinue
        }
        return @{ ExitCode = $exitCode; Output = $stdoutContent; Error = $stderrContent }
    } -ArgumentList $benchmarkExe, $benchmarkArgs

    if (Wait-Job -Job $clientJob -Timeout 60) {
        $result = Receive-Job -Job $clientJob
        $exitCode = $result.ExitCode
        $output = $result.Output
        $errorOutput = $result.Error

        if ($exitCode -eq 0) {
            Write-Host "Success."
            if (-not [string]::IsNullOrWhiteSpace($output)) {
                Write-Host "Benchmark Output:"
                Write-Host $output
            }
        } else {
            Write-Host "FAILED with exit code $exitCode"
            if (-not [string]::IsNullOrWhiteSpace($output)) {
                Write-Host "Benchmark Output:"
                Write-Host $output
            }
            if (-not [string]::IsNullOrWhiteSpace($errorOutput)) {
                Write-Host "Benchmark Error:"
                Write-Host $errorOutput
            }
        }
    } else {
        Write-Host "TIMEOUT (60s)!"
        Stop-Job -Job $clientJob -ErrorAction SilentlyContinue # Ensure job is stopped
    }

    # Stop the job and the process it was running if it's still active
    Stop-Job -Job $clientJob -ErrorAction SilentlyContinue
    # Also ensure the benchmark executable is stopped directly
    Get-Process | Where-Object { $_.Name -eq "shm_benchmark" } | Stop-Process -Force -ErrorAction SilentlyContinue


    # Stop the server process
    Write-Host "Stopping Go server (PID: $($serverProcess.Id))..."
    Stop-Process -Id $serverProcess.Id -Force -ErrorAction SilentlyContinue
    Wait-Process -Id $serverProcess.Id -ErrorAction SilentlyContinue
}

# Run benchmark cases
Run-Case -Threads 1
Run-Case -Threads 4
Run-Case -Threads 8

# Final cleanup
Cleanup-Processes

Write-Host "Done."
