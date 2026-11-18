<#
.SYNOPSIS
Checks the independent APK without installing or launching it.
.DESCRIPTION
Runs aapt2, apksigner, zipalign and llvm-readelf. Extracted libraries and
reports stay under WorkDirectory. This checks packaging, identity, signature
and ELF layout; it does not establish gameplay correctness or performance.

References:
https://developer.android.com/guide/practices/page-sizes
https://developer.android.com/tools/apksigner
.EXAMPLE
.\scripts\validate-apk.ps1 -Apk .\dist\RACiT-Android.apk -ToolchainRoot C:\path\to\toolchains
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Apk,
    [Parameter(Mandatory = $true)][string]$ToolchainRoot,
    [string]$BuildToolsVersion = '36.0.0',
    [string]$ExpectedPackage = 'com.racit.android.independent',
    [long]$ExpectedVersionCode = 9702,
    [string]$WorkDirectory = (Join-Path (Split-Path $PSScriptRoot -Parent) 'work\apk-validation'),
    [string]$ReportPath
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$apkPath = (Resolve-Path -LiteralPath $Apk).Path
$tc = (Resolve-Path -LiteralPath $ToolchainRoot).Path
if (-not (Test-Path -LiteralPath $apkPath -PathType Leaf)) { throw 'Apk must identify a file.' }
$buildTools = Join-Path $tc "android-sdk\build-tools\$BuildToolsVersion"
$aapt = Join-Path $buildTools 'aapt2.exe'
$zipalign = Join-Path $buildTools 'zipalign.exe'
$signer = Join-Path $buildTools 'lib\apksigner.jar'
$java = Join-Path $tc 'jdk-17.0.20.1+1\bin\java.exe'
$readelf = Join-Path $tc 'android-ndk-r29\toolchains\llvm\prebuilt\windows-x86_64\bin\llvm-readelf.exe'
foreach ($tool in @($aapt, $zipalign, $signer, $java, $readelf)) {
    if (-not (Test-Path -LiteralPath $tool -PathType Leaf)) { throw "Required tool missing: $tool" }
}

$runDir = Join-Path ([IO.Path]::GetFullPath($WorkDirectory)) ([Guid]::NewGuid().ToString('N'))
[IO.Directory]::CreateDirectory($runDir) | Out-Null
if (-not $ReportPath) { $ReportPath = Join-Path $runDir 'validation.json' }
$reportFile = [IO.Path]::GetFullPath($ReportPath)
$reportParent = Split-Path $reportFile -Parent
[IO.Directory]::CreateDirectory($reportParent) | Out-Null
$checks = [Collections.Generic.List[object]]::new()
$libraries = [Collections.Generic.List[object]]::new()
$failures = [Collections.Generic.List[string]]::new()

function Invoke-ValidationTool {
    param([string]$Name, [string]$Executable, [string[]]$Arguments)
    $lines = @(& $Executable @Arguments 2>&1 | ForEach-Object { "$_" })
    $exitCode = $LASTEXITCODE
    $output = $lines -join [Environment]::NewLine
    [IO.File]::WriteAllText((Join-Path $runDir "$Name.txt"), $output)
    $checks.Add([pscustomobject]@{ name = $Name; exitCode = $exitCode })
    if ($exitCode -ne 0) { $failures.Add("$Name exited with code $exitCode. See $runDir\$Name.txt") }
    return $output
}

$sha256 = (Get-FileHash -LiteralPath $apkPath -Algorithm SHA256).Hash.ToLowerInvariant()
$badging = Invoke-ValidationTool 'aapt2-badging' $aapt @('dump', 'badging', $apkPath)
$packageMatch = [regex]::Match($badging, "(?m)^package: name='([^']+)' versionCode='(\d+)'")
$actualPackage = $null
$actualVersion = $null
if ($packageMatch.Success) {
    $actualPackage = $packageMatch.Groups[1].Value
    $actualVersion = [long]$packageMatch.Groups[2].Value
    if ($actualPackage -cne $ExpectedPackage) { $failures.Add("Package is $actualPackage; expected $ExpectedPackage.") }
    if ($actualVersion -ne $ExpectedVersionCode) { $failures.Add("Version code is $actualVersion; expected $ExpectedVersionCode.") }
} else { $failures.Add('Unable to read package identity from aapt2 badging.') }
if ($badging -notmatch "(?m)^native-code: 'arm64-v8a'\s*$") {
    $failures.Add('APK must contain only the arm64-v8a native ABI.')
}

# Invoke the JAR directly so no global JAVA_HOME/PATH or batch environment is changed.
$null = Invoke-ValidationTool 'apksigner' $java @('-Xmx512m', '-jar', $signer, 'verify', '--verbose', '--print-certs', $apkPath)
$null = Invoke-ValidationTool 'zipalign' $zipalign @('-c', '-P', '16', '4', $apkPath)

Add-Type -AssemblyName System.IO.Compression.FileSystem
$archive = [IO.Compression.ZipFile]::OpenRead($apkPath)
try {
    $nativeEntries = @($archive.Entries | Where-Object { $_.FullName -match '^lib/.*\.so$' })
    if ($nativeEntries.Count -eq 0) { $failures.Add('APK has no native shared libraries.') }
    $seen = [Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
    foreach ($entry in $nativeEntries) {
        if ($entry.FullName -notmatch '^lib/arm64-v8a/([A-Za-z0-9_+.-]+\.so)$') {
            $failures.Add("Unexpected native library path: $($entry.FullName)")
            continue
        }
        $name = $Matches[1]
        if (-not $seen.Add($name)) { $failures.Add("Duplicate library entry: $name"); continue }
        if ($entry.Length -gt 1GB) { $failures.Add("Library exceeds validation extraction limit: $name"); continue }
        $libraryPath = Join-Path $runDir $name
        $inputStream = $entry.Open()
        $outputStream = [IO.File]::Create($libraryPath)
        try { $inputStream.CopyTo($outputStream) }
        finally { $outputStream.Dispose(); $inputStream.Dispose() }

        $elf = Invoke-ValidationTool ("readelf-$name") $readelf @('-h', '-l', '-W', $libraryPath)
        $elf64 = $elf -match '(?m)^\s*Class:\s+ELF64\s*$'
        $aarch64 = $elf -match '(?m)^\s*Machine:\s+AArch64\s*$'
        $shared = $elf -match '(?m)^\s*Type:\s+DYN\b'
        if (-not ($elf64 -and $aarch64 -and $shared)) { $failures.Add("$name is not an ELF64 AArch64 shared library.") }
        $loadLines = @($elf -split '\r?\n' | Where-Object { $_ -match '^\s*LOAD\s' })
        $alignments = [Collections.Generic.List[long]]::new()
        if ($loadLines.Count -eq 0) { $failures.Add("$name contains no PT_LOAD segments.") }
        foreach ($line in $loadLines) {
            $columns = $line.Trim() -split '\s+'
            try {
                $offset = [Convert]::ToInt64(($columns[1] -replace '^0x', ''), 16)
                $address = [Convert]::ToInt64(($columns[2] -replace '^0x', ''), 16)
                $alignment = [Convert]::ToInt64(($columns[-1] -replace '^0x', ''), 16)
                $alignments.Add($alignment)
                if ($alignment -lt 16384 -or ($alignment -band ($alignment - 1)) -ne 0) {
                    $failures.Add("$name has PT_LOAD alignment $alignment; requires a power of two >= 16384.")
                } elseif (($offset % $alignment) -ne ($address % $alignment)) {
                    $failures.Add("$name has inconsistent PT_LOAD offset/address alignment.")
                }
            } catch { $failures.Add("Could not parse $name program header: $line") }
        }
        $libraries.Add([pscustomobject]@{
            entry = $entry.FullName
            bytes = $entry.Length
            sha256 = (Get-FileHash -LiteralPath $libraryPath -Algorithm SHA256).Hash.ToLowerInvariant()
            elf64 = $elf64
            aarch64 = $aarch64
            sharedObject = $shared
            loadSegmentAlignment = @($alignments.ToArray())
        })
    }
    foreach ($required in @('libmain.so', 'libSDL2.so', 'libc++_shared.so')) {
        if (-not $seen.Contains($required)) { $failures.Add("Required library missing: $required") }
    }
} finally { $archive.Dispose() }

$report = [ordered]@{
    validatedAtUtc = [DateTime]::UtcNow.ToString('o')
    apk = $apkPath
    sha256 = $sha256
    package = $actualPackage
    versionCode = $actualVersion
    expectedPackage = $ExpectedPackage
    expectedVersionCode = $ExpectedVersionCode
    passed = ($failures.Count -eq 0)
    scope = 'Static APK validation only; no device, gameplay, visual, audio or performance validation.'
    checks = @($checks.ToArray())
    nativeLibraries = @($libraries.ToArray())
    failures = @($failures.ToArray())
    logsDirectory = $runDir
}
$report | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $reportFile -Encoding utf8
if ($failures.Count -gt 0) {
    $failures | ForEach-Object { Write-Host "FAIL: $_" }
    throw "APK validation failed ($($failures.Count) findings). Report: $reportFile"
}
Write-Host "APK validation passed: $actualPackage ($actualVersion), $($libraries.Count) AArch64 libraries, 16 KiB ELF/ZIP alignment, valid signature."
Write-Host "SHA-256: $sha256"
Write-Host "Report: $reportFile"
