[CmdletBinding()]
param(
    [string] $BuildDirectory = "out/vc2-portablekit-mingw",
    [string] $ElfPath = "profiles/vc2/game/portablekit-data/EBOOT.ELF",
    [string] $OutputDirectory = "profiles/vc2/generated"
)

$ErrorActionPreference = "Stop"
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..\..")).Path

function Resolve-ProjectPath([string] $Path) {
    if ([System.IO.Path]::IsPathRooted($Path)) { return $Path }
    return Join-Path $repoRoot $Path
}

$build = Resolve-ProjectPath $BuildDirectory
$elf = Resolve-ProjectPath $ElfPath
$output = Resolve-ProjectPath $OutputDirectory
$generator = Join-Path $build "portablekit\psp_recomp.exe"

if (-not (Test-Path -LiteralPath $generator -PathType Leaf)) {
    throw "PortableKit generator is missing: $generator. Configure and build target psp_recomp first."
}
if (-not (Test-Path -LiteralPath $elf -PathType Leaf)) {
    throw "Verified user-owned ELF is missing: $elf. Run VC2Native --install first."
}
if (Test-Path -LiteralPath $output) {
    $existing = Get-ChildItem -LiteralPath $output -Force | Select-Object -First 1
    if ($null -ne $existing) {
        throw "Refusing to overwrite an existing corpus at $output. Preserve it or choose a new -OutputDirectory."
    }
} else {
    New-Item -ItemType Directory -Path $output | Out-Null
}

Write-Host "Generating PortableKit AOT C++ from $elf"
& $generator $elf --auto $output
if ($LASTEXITCODE -ne 0) { throw "psp_recomp failed with exit code $LASTEXITCODE" }

Write-Host "AOT corpus generated at $output"
Write-Host "Next: cmake --build $build --target VC2Native --parallel 2"
