$ErrorActionPreference = 'Stop'
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
if (!(Test-Path -LiteralPath $vswhere)) { throw 'Visual Studio locator is unavailable' }
$installation = (& $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath | Select-Object -First 1)
if (!$installation) { throw 'MSVC x64 tools are unavailable' }
$vcvars = Join-Path $installation 'VC/Auxiliary/Build/vcvars64.bat'
if (!(Test-Path -LiteralPath $vcvars)) { throw 'MSVC developer environment script is unavailable' }
$lines = & $env:ComSpec /d /s /c "`"`"$vcvars`" >nul && set`""
if ($LASTEXITCODE -ne 0) { throw 'MSVC environment initialization failed' }
# Preserve only toolchain variables. Do not print or persist unrelated secrets.
$allowed = @('PATH','INCLUDE','LIB','LIBPATH','WindowsSdkDir','WindowsSDKVersion','VCToolsInstallDir','VSINSTALLDIR','VCINSTALLDIR','DevEnvDir','VisualStudioVersion','UCRTVersion','UniversalCRTSdkDir','WindowsLibPath','VSCMD_ARG_TGT_ARCH','VSCMD_ARG_HOST_ARCH')
foreach ($line in $lines) {
    if ($line -match '^([^=]+)=(.*)$' -and $allowed -contains $Matches[1]) {
        $name=$Matches[1]; $value=$Matches[2]
        Set-Item -Path "Env:$name" -Value $value
        if ($env:GITHUB_ENV) { "$name=$value" | Out-File -FilePath $env:GITHUB_ENV -Append -Encoding utf8 }
    }
}
Write-Host 'MSVC x64 environment initialized; SDK/compiler versions are recorded by CMake.'
