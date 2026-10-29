# Run from an x64 Developer PowerShell (MSVC + Windows SDK 10.0.22621+).
# No downloads, desktop capture, telemetry, or production app/config changes.
[CmdletBinding()]
param(
    [ValidateSet('Warp', 'HardwareSweep')][string]$Mode = 'Warp',
    [switch]$SkipBuild,
    [string]$OutputDirectory = (Join-Path $PSScriptRoot '../../out/host-backdrop-probe')
)
$ErrorActionPreference = 'Stop'
New-Item -ItemType Directory -Force $OutputDirectory | Out-Null
$output = (Resolve-Path $OutputDirectory).Path
$exe = Join-Path $output 'host-backdrop-blur.exe'
if (-not $SkipBuild) {
    if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
        throw 'Open an x64 Developer PowerShell with Windows SDK 10.0.22621+ first, or use -SkipBuild with an already built probe.'
    }
    $source = Join-Path $PSScriptRoot 'host-backdrop-blur.cpp'
    $object = Join-Path $output 'host-backdrop-blur.obj'
    & cl.exe /nologo /std:c++20 /EHsc /W4 /utf-8 /DUNICODE /D_UNICODE $source "/Fe:$exe" "/Fo:$object" /link windowsapp.lib CoreMessaging.lib d3d11.lib dxgi.lib d2d1.lib dxguid.lib dwrite.lib dwmapi.lib user32.lib gdi32.lib advapi32.lib ole32.lib
    if ($LASTEXITCODE -ne 0) { throw 'Native probe compilation failed.' }
}
if (-not (Test-Path -LiteralPath $exe -PathType Leaf)) { throw "Missing probe executable: $exe" }
Write-Host 'Only owned synthetic windows are shown. Nothing is captured or uploaded.'
Write-Host 'Exit 0 proves API/lifecycle only. Observe spatial blur yourself; zero is zero additional blur.'
foreach ($noRedirection in @($false, $true)) {
    $kind = if ($noRedirection) { 'no-redirection' } else { 'ordinary' }
    $token = [Guid]::NewGuid().ToString('N')
    $stdout = Join-Path $output "$Mode-$kind-$token.stdout.log"
    $stderr = Join-Path $output "$Mode-$kind-$token.stderr.log"
    $arguments = if ($Mode -eq 'Warp') { @('--warp', '--smoke') } else { @('--require-host', '--sweep') }
    if ($noRedirection) { $arguments += '--no-redirection' }
    Write-Host "Running $Mode / $kind; bounded to 30 seconds."
    $process = Start-Process -FilePath $exe -ArgumentList $arguments -PassThru -RedirectStandardOutput $stdout -RedirectStandardError $stderr
    try {
        $null = $process.Handle # Keep a waitable handle for Windows PowerShell 5.1 ExitCode.
        if (-not $process.WaitForExit(30000)) {
            $process.Kill()
            $process.WaitForExit()
            throw "Owned probe timed out. Logs: $stdout ; $stderr"
        }
        # Flush redirected output before reading the final process result.
        $process.WaitForExit()
        if ($process.ExitCode -ne 0) { throw "Probe exited $($process.ExitCode) (2 = canceled/invalid mode). Logs: $stdout ; $stderr" }
    } finally { $process.Dispose() }
    Get-Content -LiteralPath $stdout
    if ((Get-Item -LiteralPath $stderr).Length) { Get-Content -LiteralPath $stderr }
}
Write-Host 'Both HWND variants completed. This is not a physical blur-quality or performance certification.'
