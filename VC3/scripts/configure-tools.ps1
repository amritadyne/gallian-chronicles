[CmdletBinding()]
param(
    [string] $BuildDirectory = "out/tools",
    [string] $Compiler = ""
)

$ErrorActionPreference = "Stop"
$root = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$build = if ([IO.Path]::IsPathRooted($BuildDirectory)) { $BuildDirectory } else { Join-Path $root $BuildDirectory }

if ([string]::IsNullOrWhiteSpace($Compiler)) {
    $Compiler = (Get-Command g++.exe -ErrorAction Stop).Source
}
if (-not (Test-Path -LiteralPath $Compiler -PathType Leaf)) {
    throw "A working MinGW g++.exe is required; compiler not found: $Compiler"
}

& cmake -S $root -B $build -G Ninja `
    "-DCMAKE_CXX_COMPILER=$Compiler" `
    -DCMAKE_BUILD_TYPE=Release `
    "-DCMAKE_CXX_STANDARD_LIBRARIES=-lkernel32 -luser32 -lgdi32 -lwinspool -lshell32 -lole32 -loleaut32 -luuid -lcomdlg32 -ladvapi32" `
    -DPORTABLEKIT_RENDERER=OFF `
    -DPORTABLEKIT_FFMPEG=OFF `
    -DPSPRECOMP_BUILD_TESTS=OFF
if ($LASTEXITCODE -ne 0) { throw "CMake configure failed with exit code $LASTEXITCODE." }

& cmake --build $build --target vc3_identity_probe psp_recomp --parallel 2
if ($LASTEXITCODE -ne 0) { throw "Tool build failed with exit code $LASTEXITCODE." }

Write-Host "VC3 preparation and AOT tools are ready in $build"
