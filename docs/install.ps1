# ====================================================================
# Cob Toolchain Installer - Fixed & Patched
# ====================================================================
$ErrorActionPreference = "Stop"

# 1. Check Architecture and Assign the Exact Download URL
$RawArch = $env:PROCESSOR_ARCHITECTURE
$FallbackArch = $env:PROCESSOR_ARCHITEW6432

Write-Host "⚙️ Parsing environment architecture tokens..." -ForegroundColor Cyan

if ($RawArch -eq "AMD64" -or $FallbackArch -eq "AMD64") {
    Write-Host "✅ Detected System: 64-bit Intel/AMD (AMD64)" -ForegroundColor Green
    $ZipName = "cob-windows-amd64.zip"
    $DownloadUrl = "https://github.com/Cob-Software-Foundation/Cob/releases/latest/download/cob-windows-amd64.zip"
} 
elseif ($RawArch -eq "ARM64" -or $FallbackArch -eq "ARM64") {
    Write-Host "✅ Detected System: 64-bit ARM (ARM64)" -ForegroundColor Green
    $ZipName = "cob-windows-arm64.zip"
    $DownloadUrl = "https://github.com/Cob-Software-Foundation/Cob/releases/latest/download/cob-windows-arm64.zip"
} 
elseif ($RawArch -eq "x86") {
    Write-Host "✅ Detected System: 32-bit Intel/AMD (386)" -ForegroundColor Green
    $ZipName = "cob-windows-386.zip"
    $DownloadUrl = "https://github.com/Cob-Software-Foundation/Cob/releases/latest/download/cob-windows-386.zip"
} 
else {
    Write-Warning "⚠️ Unknown architecture variable '$RawArch'. Defaulting to amd64 target."
    $ZipName = "cob-windows-amd64.zip"
    $DownloadUrl = "https://github.com/Cob-Software-Foundation/Cob/releases/latest/download/cob-windows-amd64.zip"
}

# 2. Configure Local Target Paths
$InstallDir = Join-Path $HOME ".cob"
$ZipPath    = Join-Path ([System.IO.Path]::GetTempPath()) $ZipName

# 3. Purge Existing Directories
if (Test-Path $InstallDir) {
    Write-Host "🗑️ Cleansing old installation directory at $InstallDir..." -ForegroundColor Yellow
    Remove-Item $InstallDir -Recurse -Force
}
$null = New-Item -ItemType Directory -Force -Path $InstallDir

# 4. Fetch the Target Payload
Write-Host "🚚 Fetching artifact from release repository..." -ForegroundColor Cyan
Invoke-WebRequest -Uri $DownloadUrl -OutFile $ZipPath -UseBasicParsing

# 5. Extract Tools
Write-Host "📦 Extracting binaries to $InstallDir..." -ForegroundColor Cyan
Expand-Archive -Path $ZipPath -DestinationPath $InstallDir -Force
Remove-Item $ZipPath -Force

# 6. Apply Persistent Path Alterations
$UserPath = [Environment]::GetEnvironmentVariable("Path", "User")
if ($UserPath -notlike "*$InstallDir*") {
    Write-Host "⚙️ Injecting Cob binaries into User environment PATH..." -ForegroundColor Cyan
    $NewPath = "$UserPath;$InstallDir"
    $NewPath = $NewPath -replace ';;+', ';' # Strip redundant semicolons
    [Environment]::SetEnvironmentVariable("Path", $NewPath, "User")
    Write-Host "💾 Environment variables saved successfully!" -ForegroundColor Green
} else {
    Write-Host "ℹ️ Cob binaries are already mapped inside the current environment path." -ForegroundColor Yellow
}

Write-Host "`n🎉 Toolchain installation complete!" -ForegroundColor Green
Write-Host "🔄 Restart your PowerShell terminal session and run 'cob_interp' to check your version." -ForegroundColor Green
