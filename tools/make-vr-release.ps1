[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$Version,
    [string]$BuildDir = (Join-Path (Split-Path $PSScriptRoot -Parent) "build-afvr-on\bin\Release"),
    [string]$OutputDir = (Join-Path (Split-Path $PSScriptRoot -Parent) "dist\release"),
    [string]$Iscc,
    [switch]$SkipInstaller
)

$ErrorActionPreference = "Stop"
$repoDir = Split-Path $PSScriptRoot -Parent
$BuildDir = [System.IO.Path]::GetFullPath($BuildDir, (Get-Location).Path)
$OutputDir = [System.IO.Path]::GetFullPath($OutputDir, (Get-Location).Path)
# Keep the extracted payload path deliberately short. Red Faction's legacy
# packfile loader has a small fixed path buffer; a versioned nested directory
# can prevent alpinefaction.vpp (and therefore the D3D11 shaders) from loading.
$payloadFolderName = "VR Mod"
$payloadDir = Join-Path $OutputDir $payloadFolderName
$zipPath = Join-Path $OutputDir "AlpineFactionVR-$Version.zip"
$requiredFiles = @("AlpineFactionVR.exe", "CrashHandler.exe", "AlpineEditor.dll", "AlpineFaction.dll", "d3d8to9.dll", "alpinefaction.vpp", "licensing-info.txt")

New-Item -ItemType Directory -Force -Path $OutputDir | Out-Null
if (Test-Path -LiteralPath $payloadDir) {
    if ((Split-Path $payloadDir -Parent) -ne $OutputDir -or (Split-Path $payloadDir -Leaf) -ne $payloadFolderName) {
        throw "Refusing to clean unexpected payload path: $payloadDir"
    }
    Remove-Item -LiteralPath $payloadDir -Recurse -Force
}
New-Item -ItemType Directory -Path $payloadDir | Out-Null
foreach ($file in $requiredFiles) {
    $source = Join-Path $BuildDir $file
    if (-not (Test-Path -LiteralPath $source -PathType Leaf)) {
        $alternate = Join-Path (Split-Path $BuildDir -Parent) $file
        if (Test-Path -LiteralPath $alternate -PathType Leaf) { $source = $alternate }
        else { throw "Required release file not found: $file (looked in $BuildDir and its parent)" }
    }
    Copy-Item -LiteralPath $source -Destination $payloadDir -Force
}
Copy-Item -LiteralPath (Join-Path $repoDir "README.md") -Destination (Join-Path $payloadDir "README-VR.md") -Force
Copy-Item -LiteralPath (Join-Path $repoDir "LICENSE.txt") -Destination $payloadDir -Force
Compress-Archive -Path $payloadDir -DestinationPath $zipPath -CompressionLevel Optimal -Force
Write-Host "Created basic ZIP: $zipPath"

if (-not $SkipInstaller) {
    if (-not $Iscc) {
        $isccCandidates = @(
            (Join-Path $env:LOCALAPPDATA "Programs\Inno Setup 6\ISCC.exe"),
            (Join-Path ${env:ProgramFiles(x86)} "Inno Setup 6\ISCC.exe")
        )
        $Iscc = $isccCandidates | Where-Object { Test-Path -LiteralPath $_ -PathType Leaf } | Select-Object -First 1
    }
    if (-not (Test-Path -LiteralPath $Iscc -PathType Leaf)) { throw "Inno Setup compiler not found at '$Iscc'. Install Inno Setup 6 or pass -Iscc." }
    $patchDir = Join-Path $repoDir "setup\patches\output"
    $miniBsDiff = Join-Path $BuildDir "minibsdiff.exe"
    if (-not (Test-Path -LiteralPath $miniBsDiff -PathType Leaf)) { throw "Installer helper not found: $miniBsDiff" }
    $requiredPatches = @("patchw32.dll", "rf120_na.rtp", "rf120_gr.rtp", "rf120_fr.rtp", "RF-1.20-gr.exe.mbsdiff", "RF-1.20-fr.exe.mbsdiff", "RF-1.21.exe.mbsdiff", "RF-1.21-steam.exe.mbsdiff", "tables-gog-gr.vpp.mbsdiff")
    $missingPatches = @($requiredPatches | Where-Object { -not (Test-Path -LiteralPath (Join-Path $patchDir $_)) })
    if ($missingPatches.Count -gt 0) { throw "Installer patch payload is incomplete. Build setup/patches/output first. Missing: $($missingPatches -join ', ')" }
    $iss = Join-Path $repoDir "setup\setup.iss"
    & $Iscc "/DVRBuild" "/DAppVer=$Version" "/DBinDir=$payloadDir" "/DPatchToolDir=$BuildDir" "/O$OutputDir" $iss
    if ($LASTEXITCODE -ne 0) { throw "Inno Setup failed with exit code $LASTEXITCODE" }
    Write-Host "Created installer in: $OutputDir"
}
