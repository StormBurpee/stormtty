# Explicit diagnostic dependency preparation. Never installs into Windows/PATH.
[CmdletBinding()]
param([Parameter(Mandatory=$true)][string]$Destination)
$ErrorActionPreference = 'Stop'
$Destination = [IO.Path]::GetFullPath($Destination)
if (Test-Path -LiteralPath $Destination) { throw 'Use a fresh job-private destination' }
New-Item -ItemType Directory -Path $Destination | Out-Null
$version = '1.25.260930003'
$url = "https://api.nuget.org/v3-flatcontainer/microsoft.windows.console.conpty/$version/microsoft.windows.console.conpty.$version.nupkg"
$package = Join-Path $Destination 'conpty.nupkg'
Invoke-WebRequest -Uri $url -OutFile $package
function Assert-Hash([string]$Path, [string]$Expected) {
    $actual = (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($actual -ne $Expected) { throw "SHA256 mismatch: $Path" }
    Write-Host "$Path SHA256=$actual"
}
# Provenance: official NuGet catalog SHA512 independently matched when inspected:
# https://api.nuget.org/v3/catalog0/data/2026.10.02.21.10.18/microsoft.windows.console.conpty.1.25.260930003.json
Assert-Hash $package '02b07b349af66d801159bdf9e440d4a1ce78bb951f37fc8609731665afdae7ee'
& dotnet nuget verify $package --all --verbosity detailed
if ($LASTEXITCODE -ne 0) { throw 'NuGet signature verification failed; do not execute package' }
Add-Type -AssemblyName System.IO.Compression.FileSystem
$archive = [IO.Compression.ZipFile]::OpenRead($package)
try {
    # Exact allowlist; no general archive extraction, traversal or path search.
    $entries = @{
        'runtimes/win-x64/native/conpty.dll' = 'conpty.dll'
        'build/native/runtimes/x64/OpenConsole.exe' = 'OpenConsole.exe'
        'inc/conpty.h' = 'conpty.h'
        'Microsoft.Windows.Console.ConPTY.nuspec' = 'Microsoft.Windows.Console.ConPTY.nuspec'
    }
    foreach ($name in $entries.Keys) {
        $entry = $archive.GetEntry($name)
        if (!$entry -or $entry.Length -gt 2097152) { throw "Missing/oversized pinned entry: $name" }
        [IO.Compression.ZipFileExtensions]::ExtractToFile($entry, (Join-Path $Destination $entries[$name]), $false)
    }
} finally { $archive.Dispose() }
Assert-Hash (Join-Path $Destination 'conpty.dll') 'feeef341d891643c62d30b6b07800bc70f0bc148f44cb8c3bee84aa557ae805a'
Assert-Hash (Join-Path $Destination 'OpenConsole.exe') '3d66b23d0a71bb8eed2b77edc8b9df9bf54ce6c8fb74c863a30e760997f80586'
foreach ($name in @('conpty.dll', 'OpenConsole.exe')) {
    $file = Join-Path $Destination $name
    $signature = Get-AuthenticodeSignature -LiteralPath $file
    if ($signature.Status -ne 'Valid' -or $signature.SignerCertificate.Subject -notmatch 'O=Microsoft Corporation') {
        throw "Microsoft Authenticode validation failed for $name; do not execute"
    }
    Write-Host "$name signer=$($signature.SignerCertificate.Subject) thumbprint=$($signature.SignerCertificate.Thumbprint) version=$((Get-Item -LiteralPath $file).VersionInfo.FileVersion)"
}
# A negative packaging test can use this exact verified DLL but no host.
$missingHost = Join-Path $Destination 'missing-host'
New-Item -ItemType Directory -Path $missingHost | Out-Null
Copy-Item -LiteralPath (Join-Path $Destination 'conpty.dll') -Destination $missingHost
Write-Host "Prepared explicit experimental provider: $Destination"
Write-Host 'No provider has been executed; ordinary StormTTY defaults remain inbox.'
