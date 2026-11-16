[CmdletBinding()]
param(
    [string] $EncryptedEboot = "analysis/EBOOT.BIN",
    [string] $PreparedElf = "analysis/EBOOT.ELF",
    [string] $BuildDirectory = "out/tools"
)

$ErrorActionPreference = "Stop"
$root = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
function Resolve-WorkspacePath([string] $Path) {
    if ([IO.Path]::IsPathRooted($Path)) { return $Path }
    return Join-Path $root $Path
}

$input = Resolve-WorkspacePath $EncryptedEboot
$output = Resolve-WorkspacePath $PreparedElf
$build = Resolve-WorkspacePath $BuildDirectory
$probe = Join-Path $build "vc3_identity_probe.exe"
if (-not (Test-Path -LiteralPath $probe -PathType Leaf)) { throw "Missing $probe. Run scripts/configure-tools.ps1 first." }
if (-not (Test-Path -LiteralPath $input -PathType Leaf)) { throw "Missing extracted EBOOT.BIN: $input" }
if (Test-Path -LiteralPath $output) { throw "Refusing to overwrite existing prepared ELF: $output" }

& $probe $input $output
if ($LASTEXITCODE -ne 0) { throw "VC3 EBOOT preparation failed with exit code $LASTEXITCODE." }

$hash = (Get-FileHash -LiteralPath $output -Algorithm SHA256).Hash.ToLowerInvariant()
$identity = [ordered]@{
    disc_id = "ULJM05957"
    encrypted_eboot_sha256 = "3879146a4c4a9f31fa0435edd6c53c94e5e9e55bb838be269246cfd35e95dcbf"
    prepared_elf_sha256 = $hash
    source = "User-owned Senjou no Valkyria 3 Extra Edition Japanese disc"
} | ConvertTo-Json
Set-Content -LiteralPath (Join-Path $root "analysis/identity.json") -Value $identity -Encoding utf8
Write-Host "Prepared ELF hash recorded in analysis/identity.json: $hash"
