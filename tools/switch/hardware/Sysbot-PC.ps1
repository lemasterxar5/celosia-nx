param([Parameter(ValueFromRemainingArguments=$true)][string[]]$ClientArguments)
$ErrorActionPreference='Stop'
$python=Join-Path $env:USERPROFILE '.cache/codex-runtimes/codex-primary-runtime/dependencies/python/python.exe'
if(!(Test-Path -LiteralPath $python)) {
    $python=(Get-Command python.exe -ErrorAction Stop).Source
}
if(!$ClientArguments){$ClientArguments=@('--help')}
& $python (Join-Path $PSScriptRoot 'sysbot.py') @ClientArguments
exit $LASTEXITCODE
