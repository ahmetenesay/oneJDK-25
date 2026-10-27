# oneJDK-25 Windows Installer Script
# Downloads the JDK from GitHub Actions, extracts it, and sets ONE_JDK_25_HOME
#
# Usage:
#   .\install-onejdk-25.ps1
#   Or with custom install path:
#   .\install-onejdk-25.ps1 -InstallPath "C:\Java\oneJDK-25"

param(
    [string]$InstallPath = "$env:PROGRAMFILES\oneJDK-25",
    [string]$GitHubToken = ""
)

$ErrorActionPreference = "Stop"

Write-Host "========================================" -ForegroundColor Cyan
Write-Host "  oneJDK-25 Windows Installer" -ForegroundColor Cyan
Write-Host "  TBB-Optimized JDK 25 with Shenandoah GC" -ForegroundColor Cyan
Write-Host "========================================" -ForegroundColor Cyan
Write-Host ""

# GitHub repository details
$Repo = "ahmetenesay/oneJDK-25"
$ArtifactName = "onejdk-25-final-001-windows-x64.zip"

# Step 1: Get the latest workflow run ID
Write-Host "[1/5] Finding latest build..." -ForegroundColor Yellow

$headers = @{}
if ($GitHubToken) {
    $headers["Authorization"] = "token $GitHubToken"
} else {
    Write-Warning "No GitHub token provided. Using unauthenticated API (rate limited)."
}

try {
    # Get latest workflow run
    $runsUrl = "https://api.github.com/repos/$Repo/actions/workflows/build-onejdk-final-001.yml/runs?per_page=1"
    $runs = Invoke-RestMethod -Uri $runsUrl -Headers $headers -ErrorAction Stop
    
    if ($runs.workflow_runs.Count -eq 0) {
        Write-Error "No workflow runs found. Has the build been started?"
        exit 1
    }
    
    $runId = $runs.workflow_runs[0].id
    Write-Host "  Found workflow run: $runId" -ForegroundColor Green
    
    # Wait for completion if still running
    $status = $runs.workflow_runs[0].status
    if ($status -ne "completed") {
        Write-Host "  Build status: $status. Waiting for completion..." -ForegroundColor Yellow
        while ($status -ne "completed") {
            Start-Sleep -Seconds 30
            $run = Invoke-RestMethod -Uri "https://api.github.com/repos/$Repo/actions/runs/$runId" -Headers $headers
            $status = $run.status
            Write-Host "  Still running... ($status)" -ForegroundColor Yellow
        }
    }
    
    # Check if build succeeded
    $conclusion = $runs.workflow_runs[0].conclusion
    if ($conclusion -ne "success") {
        Write-Error "Build failed with conclusion: $conclusion"
        exit 1
    }
    Write-Host "  Build completed successfully!" -ForegroundColor Green
    
} catch {
    Write-Error "Failed to get workflow runs: $_"
    Write-Host ""
    Write-Host "Alternative: Manually download the ZIP from:" -ForegroundColor Cyan
    Write-Host "  https://github.com/$Repo/actions" -ForegroundColor Cyan
    Write-Host "Then run: .\install-onejdk-25.ps1 -ZipPath 'path\to\zip'" -ForegroundColor Cyan
    exit 1
}

# Step 2: Download the artifact
Write-Host ""
Write-Host "[2/5] Downloading oneJDK-25..." -ForegroundColor Yellow

$zipPath = "$env:TEMP\$ArtifactName"

try {
    # Get artifact download URL
    $artifactsUrl = "https://api.github.com/repos/$Repo/actions/runs/$runId/artifacts"
    $artifacts = Invoke-RestMethod -Uri $artifactsUrl -Headers $headers -ErrorAction Stop
    
    $artifact = $artifacts.artifacts | Where-Object { $_.name -eq $ArtifactName }
    if (-not $artifact) {
        Write-Error "Artifact '$ArtifactName' not found in workflow run $runId"
        exit 1
    }
    
    $downloadUrl = $artifact.archive_download_url
    $downloadUrl = $downloadUrl -replace '\{?archive_format\}?', 'zip'
    if ($GitHubToken) {
        $headers["Accept"] = "application/vnd.github+json"
    }
    
    Write-Host "  Downloading from GitHub Actions..." -ForegroundColor Gray
    Invoke-WebRequest -Uri $downloadUrl -OutFile $zipPath -Headers $headers -ErrorAction Stop
    Write-Host "  Downloaded: $ArtifactName" -ForegroundColor Green
    
} catch {
    Write-Error "Failed to download artifact: $_"
    exit 1
}

# Step 3: Extract the ZIP
Write-Host ""
Write-Host "[3/5] Extracting oneJDK-25..." -ForegroundColor Yellow

# Create install directory
if (Test-Path $InstallPath) {
    Write-Host "  Warning: $InstallPath already exists. Overwriting..." -ForegroundColor Yellow
    Remove-Item $InstallPath -Recurse -Force
}

New-Item -ItemType Directory -Path $InstallPath -Force | Out-Null

# Extract ZIP
Expand-Archive -Path $zipPath -DestinationPath $InstallPath -Force
Write-Host "  Extracted to: $InstallPath" -ForegroundColor Green

# Clean up ZIP
Remove-Item $zipPath -Force

# Step 4: Set environment variables
Write-Host ""
Write-Host "[4/5] Setting environment variables..." -ForegroundColor Yellow

# Set ONE_JDK_25_HOME (User level)
[Environment]::SetEnvironmentVariable("ONE_JDK_25_HOME", $InstallPath, "User")
Write-Host "  ONE_JDK_25_HOME = $InstallPath" -ForegroundColor Green

# Add to PATH (User level)
$currentPath = [Environment]::GetEnvironmentVariable("Path", "User")
$binPath = "$InstallPath\bin"
if ($currentPath -notlike "*$binPath*") {
    $newPath = "$currentPath;$binPath"
    [Environment]::SetEnvironmentVariable("Path", $newPath, "User")
    Write-Host "  Added to PATH: $binPath" -ForegroundColor Green
} else {
    Write-Host "  Already in PATH: $binPath" -ForegroundColor Gray
}

# Broadcast environment change to other processes
$shell = New-Object -ComObject Shell.Application
$shell.Namespace(0).Self.InvokeVerb("RefreshEnvironment") 2>$null

Write-Host "  Environment variables set!" -ForegroundColor Green
Write-Host "  NOTE: Open a new PowerShell window to use the new variables." -ForegroundColor Yellow

# Step 5: Verify installation
Write-Host ""
Write-Host "[5/5] Verifying installation..." -ForegroundColor Yellow

$javaExe = "$InstallPath\bin\java.exe"
if (-not (Test-Path $javaExe)) {
    Write-Error "java.exe not found at $javaExe"
    exit 1
}

# Test Java version
Write-Host "  Testing Java version..." -ForegroundColor Gray
$javaVersion = & $javaExe -version 2>&1
Write-Host "  $javaVersion" -ForegroundColor Green

# Test Shenandoah GC
Write-Host "  Testing Shenandoah GC..." -ForegroundColor Gray
$shenandoahTest = & $javaExe -XX:+UseShenandoahGC -version 2>&1
if ($LASTEXITCODE -eq 0) {
    Write-Host "  Shenandoah GC: OK" -ForegroundColor Green
} else {
    Write-Warning "Shenandoah GC test failed. Check installation."
}

Write-Host ""
Write-Host "========================================" -ForegroundColor Cyan
Write-Host "  oneJDK-25 Installation Complete!" -ForegroundColor Cyan
Write-Host "========================================" -ForegroundColor Cyan
Write-Host ""
Write-Host "Install path: $InstallPath" -ForegroundColor White
Write-Host "ONE_JDK_25_HOME: $InstallPath" -ForegroundColor White
Write-Host ""
Write-Host "Next steps:" -ForegroundColor Yellow
Write-Host "  1. Open a new PowerShell window" -ForegroundColor White
Write-Host "  2. Test: echo `$env:ONE_JDK_25_HOME" -ForegroundColor White
Write-Host "  3. Test: java -version" -ForegroundColor White
Write-Host "  4. For JingZero: Set ONE_JDK_25_HOME in build.gradle or use the environment variable" -ForegroundColor White
Write-Host ""
