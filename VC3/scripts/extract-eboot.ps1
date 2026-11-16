[CmdletBinding()]
param(
    [string] $IsoPath = "Senjou no Valkyria 3 E2 - Extra Edition (Japan).iso",
    [string] $OutputDirectory = "analysis"
)

$ErrorActionPreference = "Stop"
$root = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
function Resolve-WorkspacePath([string] $Path) {
    if ([IO.Path]::IsPathRooted($Path)) { return $Path }
    return Join-Path $root $Path
}

$iso = Resolve-WorkspacePath $IsoPath
$output = Resolve-WorkspacePath $OutputDirectory
if (-not (Test-Path -LiteralPath $iso -PathType Leaf)) { throw "ISO not found: $iso" }
foreach ($name in @("EBOOT.BIN", "PARAM.SFO")) {
    if (Test-Path -LiteralPath (Join-Path $output $name)) {
        throw "Refusing to overwrite existing $name in $output"
    }
}
New-Item -ItemType Directory -Path $output -Force | Out-Null

$sectorBytes = 2048
$stream = [IO.File]::Open($iso, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read)
function Read-IsoBytes([Int64] $Offset, [int] $Count) {
    $buffer = New-Object byte[] $Count
    [void] $stream.Seek($Offset, [IO.SeekOrigin]::Begin)
    $read = 0
    while ($read -lt $Count) {
        $current = $stream.Read($buffer, $read, $Count - $read)
        if ($current -le 0) { throw "Unexpected end of ISO." }
        $read += $current
    }
    return ,$buffer
}
function Read-IsoDirectory([UInt32] $Extent, [UInt32] $Size) {
    $bytes = Read-IsoBytes ([Int64] $Extent * $sectorBytes) ([int] $Size)
    $entries = @{}
    $offset = 0
    while ($offset -lt $bytes.Length) {
        $length = $bytes[$offset]
        if ($length -eq 0) {
            $offset = ([int] [Math]::Floor($offset / $sectorBytes) + 1) * $sectorBytes
            continue
        }
        if ($offset + $length -gt $bytes.Length) { break }
        $nameLength = $bytes[$offset + 32]
        $name = [Text.Encoding]::ASCII.GetString($bytes, $offset + 33, $nameLength) -replace ';1$', ''
        if ($name -notin @([string] [char] 0, [string] [char] 1)) {
            $entries[$name] = [pscustomobject]@{
                Extent = [BitConverter]::ToUInt32($bytes, $offset + 2)
                Size = [BitConverter]::ToUInt32($bytes, $offset + 10)
            }
        }
        $offset += $length
    }
    return $entries
}
function Write-IsoFile($Entry, [string] $Path) {
    $bytes = Read-IsoBytes ([Int64] $Entry.Extent * $sectorBytes) ([int] $Entry.Size)
    [IO.File]::WriteAllBytes($Path, $bytes)
}

try {
    $pvd = Read-IsoBytes ([Int64] 16 * $sectorBytes) $sectorBytes
    if ([Text.Encoding]::ASCII.GetString($pvd, 1, 5) -ne "CD001") { throw "ISO is not ISO-9660." }
    $rootEntries = Read-IsoDirectory ([BitConverter]::ToUInt32($pvd, 158)) ([BitConverter]::ToUInt32($pvd, 166))
    $psp = $rootEntries["PSP_GAME"]
    if ($null -eq $psp) { throw "PSP_GAME directory is missing." }
    $pspEntries = Read-IsoDirectory $psp.Extent $psp.Size
    $system = $pspEntries["SYSDIR"]
    $sfo = $pspEntries["PARAM.SFO"]
    if ($null -eq $system -or $null -eq $sfo) { throw "PSP_GAME/SYSDIR or PARAM.SFO is missing." }
    $systemEntries = Read-IsoDirectory $system.Extent $system.Size
    $eboot = $systemEntries["EBOOT.BIN"]
    if ($null -eq $eboot) { throw "PSP_GAME/SYSDIR/EBOOT.BIN is missing." }

    Write-IsoFile $eboot (Join-Path $output "EBOOT.BIN")
    Write-IsoFile $sfo (Join-Path $output "PARAM.SFO")
    $hash = (Get-FileHash -LiteralPath (Join-Path $output "EBOOT.BIN") -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($hash -ne "3879146a4c4a9f31fa0435edd6c53c94e5e9e55bb838be269246cfd35e95dcbf") {
        throw "Extracted EBOOT.BIN does not match the supported ULJM-05957 Extra Edition hash: $hash"
    }
    Write-Host "Extracted and verified EBOOT.BIN and PARAM.SFO in $output"
} finally {
    $stream.Dispose()
}
