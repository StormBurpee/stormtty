$ErrorActionPreference = 'Stop'
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
if (!(Test-Path -LiteralPath $vswhere)) { throw 'Visual Studio locator is unavailable' }
$installation = (& $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath | Select-Object -First 1)
if (!$installation) { throw 'MSVC x64 tools are unavailable' }
$vcvars = Join-Path $installation 'VC/Auxiliary/Build/vcvars64.bat'
if (!(Test-Path -LiteralPath $vcvars)) { throw 'MSVC developer environment script is unavailable' }
# Run a temporary batch file through ProcessStartInfo, avoiding PowerShell's
# native argument re-quoting of the space-containing Visual Studio path.
$temporaryRoot = if ($env:RUNNER_TEMP) { $env:RUNNER_TEMP } else { [IO.Path]::GetTempPath() }
$batch = Join-Path $temporaryRoot ("stormtty-msvc-" + [Guid]::NewGuid().ToString('N') + '.cmd')
try {
    [IO.File]::WriteAllLines($batch, @('@echo off', ('call "' + $vcvars + '" >nul'), 'if errorlevel 1 exit /b 1', 'set'), [Text.Encoding]::ASCII)
    $start = [Diagnostics.ProcessStartInfo]::new()
    $start.FileName = $env:ComSpec
    $start.Arguments = '/d /c "' + $batch + '"'
    $start.UseShellExecute = $false
    $start.RedirectStandardOutput = $true
    $start.RedirectStandardError = $true
    $process = [Diagnostics.Process]::Start($start)
    $stdout = $process.StandardOutput.ReadToEndAsync()
    $stderr = $process.StandardError.ReadToEndAsync()
    if (!$process.WaitForExit(60000)) {
        $process.Kill($true)
        throw 'MSVC environment initialization timed out'
    }
    $output = $stdout.GetAwaiter().GetResult()
    $errorOutput = $stderr.GetAwaiter().GetResult()
    if ($process.ExitCode -ne 0) { throw ('MSVC environment initialization failed: ' + $errorOutput) }
    $lines = $output -split "`r?`n"
} finally {
    if (Test-Path -LiteralPath $batch) { Remove-Item -LiteralPath $batch -Force }
}
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
