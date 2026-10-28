$ErrorActionPreference = 'Stop'
if (Get-Command py -ErrorAction SilentlyContinue) {
    & py -3 "$PSScriptRoot/bootstrap.py" @args
} else {
    & python "$PSScriptRoot/bootstrap.py" @args
}
exit $LASTEXITCODE
