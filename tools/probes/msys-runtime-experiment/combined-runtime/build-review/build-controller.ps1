param([Parameter(Mandatory=$true)][string]$Output)
$ErrorActionPreference = 'Stop'
if ([IO.Path]::GetFullPath($Output) -cne 'C:\stormtty-build-review\controller') { throw 'Noncanonical controller output' }
if ($PSScriptRoot -cne 'C:\stormtty-build-review\source\tools\probes\msys-runtime-experiment\combined-runtime\build-review') { throw 'Noncanonical source root' }
if (Test-Path $Output) { throw 'New private controller output required' }
New-Item -ItemType Directory -Path $Output | Out-Null
$Output = (Resolve-Path $Output).Path
& python "$PSScriptRoot/source-manifest.py" "$Output/source-before.json"
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
$source = (Resolve-Path "$PSScriptRoot/../native/controller.c").Path
$compiler = (Get-Command cl.exe).Source
$linker = (Get-Command link.exe).Source
function Tool-Snapshot {
  $paths = @((Split-Path $compiler), (Split-Path $linker)) | Sort-Object -Unique
  @($paths | ForEach-Object { Get-ChildItem -LiteralPath $_ -File | Where-Object { $_.Extension -in '.exe','.dll' } } |
    Sort-Object FullName -Unique | ForEach-Object {
      [ordered]@{path=$_.FullName;sha256=(Get-FileHash $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant()}
    }) | ConvertTo-Json -Depth 4
}
[ordered]@{compiler=$compiler;linker=$linker;include=$env:INCLUDE;lib=$env:LIB;
  compiler_flags='/nologo /std:c11 /W4 /WX /O2 /Brepro /c';linker_flags='/nologo /WX /Brepro kernel32.lib';
  source_root=$PSScriptRoot;output=$Output
} | ConvertTo-Json | Set-Content "$Output/build-config.json"
Tool-Snapshot | Set-Content "$Output/tool-binaries-before.json"
& cl.exe /? 2>&1 | Set-Content "$Output/compiler-help.txt"
& link.exe /? 2>&1 | Set-Content "$Output/linker-help.txt"
(Get-FileHash $compiler -Algorithm SHA256).Hash.ToLowerInvariant() | Set-Content "$Output/compiler-sha256.txt"
# Compile/link only. Never launch this executable in a build-review job.
& cl.exe /nologo /std:c11 /W4 /WX /O2 /Brepro /c $source "/Fo$Output/controller.obj" 2>&1 | Tee-Object "$Output/compiler.log"
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& link.exe /nologo /WX /Brepro "$Output/controller.obj" "/OUT:$Output/controller.exe" kernel32.lib 2>&1 | Tee-Object "$Output/link.log"
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& dumpbin.exe /headers /imports "$Output/controller.exe" | Set-Content "$Output/controller-pe.txt"
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
if (-not (Select-String -LiteralPath "$Output/controller-pe.txt" -Pattern '\bREPRO\b' -Quiet)) { throw 'Reproducible-link debug entry absent; unsupported control' }
& dumpbin.exe /disasm "$Output/controller.exe" | Set-Content "$Output/controller-disassembly.txt"
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& dumpbin.exe /symbols "$Output/controller.obj" | Set-Content "$Output/controller-symbols.txt"
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
Tool-Snapshot | Set-Content "$Output/tool-binaries-after.json"
if ((Get-Content -Raw "$Output/tool-binaries-before.json") -cne (Get-Content -Raw "$Output/tool-binaries-after.json")) { throw 'Compiler/linker tool drift' }
& python "$PSScriptRoot/source-manifest.py" "$Output/source-before.json" --check
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
[ordered]@{schema=2; scope='controller-build-only'; fixtures_executed=$false; review_required=$true;
  controller_sha256=(Get-FileHash "$Output/controller.exe" -Algorithm SHA256).Hash.ToLowerInvariant();
  object_sha256=(Get-FileHash "$Output/controller.obj" -Algorithm SHA256).Hash.ToLowerInvariant();
  source_sha256=(Get-FileHash $source -Algorithm SHA256).Hash.ToLowerInvariant()
} | ConvertTo-Json | Set-Content "$Output/controller-build-review.json"
