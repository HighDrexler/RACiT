[CmdletBinding()]
param(
    # The folder holding android-ndk-r29, android-sdk, gradle-8.13 and jdk-17.0.20.1+1 (or set RACIT_ANDROID_TOOLCHAINS).
    [string]$ToolchainsRoot = $env:RACIT_ANDROID_TOOLCHAINS,
    [string]$HostToolsRoot = '',
    [string]$PythonExecutable = '',
    [string]$StateRoot = '',
    [ValidateRange(1,24)][int]$Jobs = 4,
    [switch]$NativeOnly,
    [switch]$PackageOnly
)
$ErrorActionPreference = 'Stop'
if (!$ToolchainsRoot) { throw 'Pass -ToolchainsRoot (android-ndk-r29, android-sdk, gradle-8.13, jdk-17.0.20.1+1) or set RACIT_ANDROID_TOOLCHAINS.' }
$root = Split-Path $PSScriptRoot -Parent
# The build's own state (native objects, Gradle and Android user homes): -StateRoot, else work/ beside the repository.
if ($StateRoot) { $state = [IO.Path]::GetFullPath($StateRoot) }
else { $state = Join-Path (Split-Path $root -Parent) 'racit-android-work' }
if (!$HostToolsRoot) { $HostToolsRoot = Join-Path $state 'toolchains' }
$build = Join-Path $state 'native'
$android = Join-Path $root 'android'
$versionSource = Get-Content -LiteralPath (Join-Path $android 'app\build.gradle') -Raw
$versionCodeMatch = [regex]::Match($versionSource, '(?m)^\s*versionCode\s+(\d+)\s*$')
$versionNameMatch = [regex]::Match($versionSource, "(?m)^\s*versionName\s+'([A-Za-z0-9._-]+)'\s*$")
if (!$versionCodeMatch.Success -or !$versionNameMatch.Success) { throw 'Cannot determine the APK version from app/build.gradle.' }
$versionCode = [long]$versionCodeMatch.Groups[1].Value
$versionName = $versionNameMatch.Groups[1].Value
$jni = Join-Path $android 'app\src\main\jniLibs\arm64-v8a'
$ndk = Join-Path $ToolchainsRoot 'android-ndk-r29'
$llvm = Join-Path $ndk 'toolchains\llvm\prebuilt\windows-x86_64'
$cmake = Join-Path $HostToolsRoot 'cmake\bin\cmake.exe'
$ninja = Join-Path $HostToolsRoot 'ninja.exe'
$gradle = Join-Path $ToolchainsRoot 'gradle-8.13\bin\gradle.bat'
$jdk = Join-Path $ToolchainsRoot 'jdk-17.0.20.1+1'
$sdk = Join-Path $ToolchainsRoot 'android-sdk'
# Refuse to borrow another source tree's mutable build state, including when
# PackageOnly bypasses configuration. A moved project must use -StateRoot.
$cachePath = Join-Path $build 'CMakeCache.txt'
if (Test-Path -LiteralPath $cachePath) {
    $homeLine = Get-Content -LiteralPath $cachePath | Where-Object { $_ -like 'CMAKE_HOME_DIRECTORY:INTERNAL=*' } | Select-Object -First 1
    $expectedHome = [IO.Path]::GetFullPath((Join-Path $android 'app\src\main\cpp')).Replace('\','/').TrimEnd('/')
    if (!$homeLine -or $homeLine.Substring('CMAKE_HOME_DIRECTORY:INTERNAL='.Length).TrimEnd('/') -ine $expectedHome) {
        throw 'Native state belongs to a different source tree. Select a new private directory with -StateRoot.'
    }
}
if (!$PythonExecutable) {
    $pythonConfig = Join-Path $HostToolsRoot 'python-path.txt'
    if (Test-Path -LiteralPath $pythonConfig) { $PythonExecutable = (Get-Content -LiteralPath $pythonConfig -Raw).Trim() }
    else { $PythonExecutable = (Get-Command python -ErrorAction Stop).Source }
}
& $PythonExecutable -c 'import sys; sys.exit(0 if sys.version_info >= (3, 10) else 1)'
if ($LASTEXITCODE -ne 0) { throw 'Python 3.10 or newer is required. Pass -PythonExecutable with its full path.' }
foreach ($required in @($cmake,$ninja,$gradle,(Join-Path $jdk 'bin\java.exe'),(Join-Path $ndk 'build\cmake\android.toolchain.cmake'))) {
    if (!(Test-Path -LiteralPath $required)) { throw "Missing build tool: $required" }
}
function Invoke-Checked([string]$Exe, [string[]]$Arguments) {
    & $Exe @Arguments
    if ($LASTEXITCODE -ne 0) { throw "$Exe failed ($LASTEXITCODE)" }
}
New-Item -ItemType Directory -Force -Path $build,$jni,(Join-Path $state 'temp'),(Join-Path $state 'android-user') | Out-Null
$env:JAVA_HOME = $jdk
$env:ANDROID_HOME = $sdk
$env:ANDROID_SDK_ROOT = $sdk
$env:GRADLE_USER_HOME = Join-Path $HostToolsRoot 'gradle-cache'
$env:ANDROID_USER_HOME = Join-Path $state 'android-user'
$env:TEMP = Join-Path $state 'temp'
$env:TMP = $env:TEMP
$env:PYTHONDONTWRITEBYTECODE = '1'
$env:PATH = (Join-Path $jdk 'bin') + ';' + $env:PATH
# Always configure against this physical snapshot. Never reuse another checkout's cache.
if (!$PackageOnly) {
    Invoke-Checked $cmake @('-S',(Join-Path $android 'app\src\main\cpp'),'-B',$build,'-G','Ninja',
        "-DCMAKE_MAKE_PROGRAM=$($ninja.Replace('\','/'))",
        "-DCMAKE_TOOLCHAIN_FILE=$($ndk.Replace('\','/'))/build/cmake/android.toolchain.cmake",
        "-DPython3_EXECUTABLE=$($PythonExecutable.Replace('\','/'))",
        '-DANDROID_ABI=arm64-v8a','-DANDROID_PLATFORM=android-29','-DANDROID_STL=c++_shared',
        '-DANDROID_SUPPORT_FLEXIBLE_PAGE_SIZES=ON','-DCMAKE_BUILD_TYPE=Release','-DCMAKE_OBJECT_PATH_MAX=240')
    Invoke-Checked $cmake @('--build',$build,'--parallel',"$Jobs",'--target','RaCACiTRecomp','SDL2')
}
foreach ($library in @(@('game\libmain.so','libmain.so'),@('SDL2\libSDL2.so','libSDL2.so'))) {
    $inputFile = Join-Path $build $library[0]
    if (!(Test-Path -LiteralPath $inputFile)) { throw "Build native code first: $inputFile" }
    Invoke-Checked (Join-Path $llvm 'bin\llvm-strip.exe') @('--strip-unneeded',$inputFile,'-o',(Join-Path $jni $library[1]))
}
Copy-Item -LiteralPath (Join-Path $llvm 'sysroot\usr\lib\aarch64-linux-android\libc++_shared.so') -Destination $jni
if ($NativeOnly) { Write-Output 'Native build completed.'; exit 0 }
$key = Join-Path $android 'racit-independent.keystore'
if (!(Test-Path -LiteralPath $key)) {
    Invoke-Checked (Join-Path $jdk 'bin\keytool.exe') @('-genkeypair','-keystore',$key,'-storepass','android','-keypass','android',
        '-alias','racacit','-keyalg','RSA','-keysize','3072','-validity','10000','-dname','CN=RACiT Independent Android Development')
}
[IO.File]::WriteAllText((Join-Path $android 'local.properties'),"sdk.dir=$($sdk.Replace('\','/'))`n")
Push-Location $android
try { Invoke-Checked $gradle @('--offline','--no-daemon','--max-workers=2','--console=plain','assembleRelease') }
finally { Pop-Location }
$artifacts = Join-Path $root 'artifacts'
New-Item -ItemType Directory -Path $artifacts -Force | Out-Null
$apk = Join-Path $artifacts "RACiT-$versionName-arm64.apk"
Copy-Item -LiteralPath (Join-Path $android 'app\build\outputs\apk\release\app-release.apk') -Destination $apk
$hash = Get-FileHash -LiteralPath $apk -Algorithm SHA256
"$($hash.Hash.ToLowerInvariant())  $([IO.Path]::GetFileName($apk))" | Set-Content -LiteralPath "$apk.sha256" -Encoding ascii
& (Join-Path $PSScriptRoot 'validate-apk.ps1') -Apk $apk -ToolchainRoot $ToolchainsRoot -ExpectedVersionCode $versionCode -WorkDirectory (Join-Path $state 'apk-validation') -ReportPath (Join-Path $artifacts 'apk-validation.json')
# Keep matching debug symbols outside the APK for real device crash/profile
# diagnosis. An incremental build must not silently erase the last release's
# only usable address-to-source mapping.
$symbols = Join-Path $artifacts "symbols\$versionName"
New-Item -ItemType Directory -Force -Path $symbols | Out-Null
$symbolRecords = @()
foreach ($library in @(@('game\libmain.so','libmain.so'),@('SDL2\libSDL2.so','libSDL2.so'))) {
    $inputFile = Join-Path $build $library[0]
    $debugFile = Join-Path $symbols ($library[1] + '.debug')
    Invoke-Checked (Join-Path $llvm 'bin\llvm-objcopy.exe') @('--only-keep-debug','--compress-debug-sections=zlib',$inputFile,$debugFile)
    $buildId = (& (Join-Path $llvm 'bin\llvm-readelf.exe') --notes $inputFile | Select-String 'Build ID:').ToString().Trim()
    $symbolRecords += [pscustomobject]@{ library=$library[1]; buildId=$buildId; path=$debugFile; sha256=(Get-FileHash -LiteralPath $debugFile -Algorithm SHA256).Hash.ToLowerInvariant() }
}
[pscustomobject]@{ builtAtUtc=[DateTime]::UtcNow.ToString('o'); package='com.racit.android.independent'; version=$versionName;
    versionCode=$versionCode; apkSha256=$hash.Hash.ToLowerInvariant(); symbols=$symbolRecords } |
    ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $artifacts 'build-receipt.json') -Encoding utf8
Write-Output "APK: $apk"
