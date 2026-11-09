# Rebuilds the recomp: function list -> N64Recomp -> clang++ -> recomp\buriki.exe
# Run from anywhere:  powershell -ExecutionPolicy Bypass -File D:\Buriki\tools\build_recomp.ps1
#   -Ghidra   also rerun Ghidra's function discovery (slow, ~1-2 min)
#   -Opt O2   optimisation level (default O1)
#   -Cores    runtime subfolders to include (default: iomcu = I/O board MCU, audiocpu = sound CPU,
#             frontend = window, Direct3D 11, pause menu, settings)
param([switch]$Ghidra, [string]$Opt = 'O1', [string[]]$Cores = @('iomcu', 'audiocpu', 'frontend'))
$ErrorActionPreference = 'Stop'
$root = 'D:\Buriki'
$rt = "$root\recomp\runtime"

if (-not (Test-Path "$root\work\buriki_program_be.bin")) {
    python -I "$root\tools\build_image.py" "$root\roms" "$root\work\buriki_program_be.bin" | Out-Null
}
$b = [IO.File]::ReadAllBytes("$root\work\buriki_program_be.bin")
[IO.File]::WriteAllBytes("$root\work\buriki_code_80000000.bin", $b[0x100000..0x1FFFFF])

if ($Ghidra) {
    & "$root\third_party\ghidra_12.2_PUBLIC\support\analyzeHeadless.bat" "$root\work\ghidra" buriki -overwrite `
        -import "$root\work\buriki_code_80000000.bin" -loader BinaryLoader -loader-baseAddr 0x80000000 `
        -processor "MIPS:BE:64:64-32addr" -scriptPath "$root\tools\ghidra_scripts" `
        -postScript SeedAndExport.java "$root\work\functions_raw.txt" | Out-Null
}

if (-not (Test-Path "$root\work\bios_9fc00000.bin")) { Copy-Item "$root\roms\hng64\brom1.bin" "$root\work\bios_9fc00000.bin" }
if ($Ghidra) {
    & "$root\third_party\ghidra_12.2_PUBLIC\support\analyzeHeadless.bat" "$root\work\ghidra" buriki -overwrite `
        -import "$root\work\bios_9fc00000.bin" -loader BinaryLoader -loader-baseAddr 0x9FC00000 `
        -processor "MIPS:BE:64:64-32addr" -scriptPath "$root\tools\ghidra_scripts" `
        -postScript SeedAndExport.java "$root\work\bios_functions_raw.txt" "$root\tools\bios_seeds.txt" | Out-Null
}

Push-Location "$root\recomp"
try {
    # -I would hide tools\ from Python's import path, so these two run without it.
    python "$root\tools\make_symbols.py" buriki.syms.toml
    python "$root\tools\find_kernel_funcs.py" buriki.syms.toml kernel_funcs.txt
    if ($LASTEXITCODE) { throw "kernel function check failed" }
    python -I "$root\tools\make_func_table.py" buriki.syms.toml "$rt\func_table.cpp"
    & "$root\tools\make_recomp_header.ps1"
    # Run through cmd so warnings on stderr are logged rather than treated as PowerShell errors.
    cmd /c "`"$root\third_party\N64Recomp\build\N64Recomp.exe`" buriki.toml > recomp_log.txt 2>&1"
    if ($LASTEXITCODE) { Get-Content recomp_log.txt -Tail 20; throw "N64Recomp failed, see recomp\recomp_log.txt" }
    Get-Content recomp_log.txt | Select-String '\[Warn\]|Error' | ForEach-Object { "  recomp: $($_.Line)" }
    python -I "$root\tools\fix_generated.py" generated

    New-Item -ItemType Directory -Force build_obj | Out-Null
    # Runtime: top-level files plus the subfolders listed in $Cores
    # (iomcu = I/O board MCU, audiocpu = sound CPU, frontend = PC front end).
    $files = @(Get-ChildItem generated -Filter funcs_*.c) + @(Get-ChildItem $rt -Filter *.cpp)
    foreach ($sub in $Cores) { if (Test-Path "$rt\$sub") { $files += @(Get-ChildItem "$rt\$sub" -Filter *.cpp) } }
    # Object names include the subfolder so files with the same name don't collide.
    $objOf = { param($f) "$root\recomp\build_obj\" + (($f.FullName.Substring("$root\recomp\".Length)) -replace '[\\/]', '_' -replace '\.[^.]+$', '.o') }
    $sw = [Diagnostics.Stopwatch]::StartNew()
    $jobs = $files | ForEach-Object {
        Start-Job -ScriptBlock {
            param($p, $o, $rt, $gen, $opt)
            if ($p -like "$rt\*") { $opt = 'O2' }   # the hand-written runtime (renderer) is always optimised
            $r = & clang++ -x c++ -std=c++20 "-$opt" -w -I $rt -I $gen -c $p -o $o 2>&1 | ForEach-Object { "$_" }
            [pscustomobject]@{ File = (Split-Path $p -Leaf); Exit = $LASTEXITCODE; Err = (($r | Select-Object -First 12) -join "`n") }
        } -ArgumentList $_.FullName, (& $objOf $_), $rt, "$root\recomp\generated", $Opt
    }
    $res = $jobs | Wait-Job | Receive-Job; $jobs | Remove-Job
    $bad = $res | Where-Object Exit -ne 0
    if ($bad) { $bad | Format-List File, Err; throw "$($bad.Count) file(s) failed to compile" }
    "compiled $($res.Count) files in $([int]$sw.Elapsed.TotalSeconds)s"

    $objs = $files | ForEach-Object { & $objOf $_ }
    # d3d11/dxgi: presentation; ole32: menu art (WIC). The shader compiler (d3dcompiler_47.dll) is loaded at run time.
    & clang++ $objs -o buriki.exe -luser32 -lgdi32 -lwinmm -ld3d11 -ldxgi -lole32
    if ($LASTEXITCODE) { throw "link failed" }
    "built recomp\buriki.exe"
} finally { Pop-Location }
