[CmdletBinding()]
param(
    [string] $BuildDirectory = "out/tools",
    [string] $ElfPath = "analysis/EBOOT.ELF",
    [string] $OutputDirectory = "generated"
)

$ErrorActionPreference = "Stop"
$root = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
function Resolve-WorkspacePath([string] $Path) {
    if ([IO.Path]::IsPathRooted($Path)) { return $Path }
    return Join-Path $root $Path
}

$build = Resolve-WorkspacePath $BuildDirectory
$elf = Resolve-WorkspacePath $ElfPath
$output = Resolve-WorkspacePath $OutputDirectory
$generator = Join-Path $build "portablekit/psp_recomp.exe"
if (-not (Test-Path -LiteralPath $generator -PathType Leaf)) { throw "Missing $generator. Run scripts/configure-tools.ps1 first." }
if (-not (Test-Path -LiteralPath $elf -PathType Leaf)) { throw "Missing prepared ELF: $elf" }
if (Test-Path -LiteralPath $output) {
    if (Get-ChildItem -LiteralPath $output -Force | Select-Object -First 1) {
        throw "Refusing to overwrite an existing generated corpus: $output"
    }
}
New-Item -ItemType Directory -Path $output -Force | Out-Null

& $generator $elf --auto $output
if ($LASTEXITCODE -ne 0) { throw "psp_recomp failed with exit code $LASTEXITCODE." }
Write-Host "VC3 C++ corpus generated at $output"
