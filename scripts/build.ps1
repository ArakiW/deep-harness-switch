# Cross-compile .nro using native Windows clang + ld.lld + a local devkitA64 sysroot.
# NOTE: keep this file pure ASCII - Windows PowerShell 5.1 misparses UTF-8
# scripts without BOM when they contain non-ASCII comments.
$ErrorActionPreference = 'Stop'

# switch-tools exes (nacptool/elf2nro) need msys64 runtime DLLs.
$msysBin = if ($env:MSYS2_BIN) { $env:MSYS2_BIN } else { 'C:\msys64\usr\bin' }
if (Test-Path $msysBin) { $env:PATH = "$msysBin;$env:PATH" }

$repo = Split-Path -Parent $PSScriptRoot
# DSH_SWITCH_DEPS must point at the folder that contains both
# .tools\devkita64-sysroot and .tools\switch-tools-win.
$fw = $env:DSH_SWITCH_DEPS
if (-not $fw) { throw 'DSH_SWITCH_DEPS is not set; point it at the folder containing .tools\devkita64-sysroot and .tools\switch-tools-win' }
$toolRoot    = "$fw\.tools\devkita64-sysroot\opt\devkitpro"
$switchTools = "$fw\.tools\switch-tools-win\bin"
$llvm        = if ($env:LLVM_BIN) { $env:LLVM_BIN } else { 'C:\Program Files\LLVM\bin' }
$build       = Join-Path $repo 'build'
$portlib     = Join-Path $toolRoot 'portlibs\switch\lib'

$gccVer = Get-ChildItem (Join-Path $toolRoot 'devkitA64\lib\gcc\aarch64-none-elf') -Directory |
          Sort-Object Name -Descending | Select-Object -First 1
$gccPic    = Join-Path $gccVer.FullName 'pic'
$newlibPic = Join-Path $toolRoot 'devkitA64\aarch64-none-elf\lib\pic'

New-Item -ItemType Directory -Force -Path $build | Out-Null

$common = @(
    '--target=aarch64-none-elf','-D__SWITCH__','-D_REENTRANT',
    '-march=armv8-a+crc+crypto','-mcpu=cortex-a57','-mtp=tpidrro_el0',
    '-O2','-g','-fPIE','-ffunction-sections','-fdata-sections','-fno-strict-aliasing',
    '-Wall','-Wextra',
    '-isystem',(Join-Path $toolRoot 'libnx\include'),
    '-isystem',(Join-Path $toolRoot 'devkitA64\aarch64-none-elf\include'),
    '-isystem',(Join-Path $toolRoot 'portlibs\switch\include'),
    '-isystem',(Join-Path $toolRoot 'portlibs\switch\include\SDL2'),
    '-isystem',(Join-Path $toolRoot 'portlibs\switch\include\freetype2'),
    '-I',(Join-Path $repo 'source')
)

$objects = @()

# 编译源文件(当前仅 main.c)
$src = Join-Path $repo 'source\main.c'
$obj = Join-Path $build 'main.o'
Write-Host 'compiling source\main.c'
& (Join-Path $llvm 'clang.exe') @common -c $src -o $obj
if ($LASTEXITCODE -ne 0) { throw 'compile failed: source\main.c' }
$objects += $obj

Push-Location $build
try {
    & (Join-Path $llvm 'llvm-ar.exe') x (Join-Path $toolRoot 'libnx\lib\libnx.a') switch_crt0.o
    if ($LASTEXITCODE -ne 0) { throw 'extract switch_crt0.o failed' }
} finally { Pop-Location }

# Embed assets into rodata via objcopy (romfs support in this elf2nro is unreliable).
$fontSrc = Join-Path $repo 'assets\NotoSansCJKsc-Regular.otf'
$fontObj = Join-Path $build 'noto_font.o'
Push-Location (Split-Path $fontSrc)
try {
    & (Join-Path $llvm 'llvm-objcopy.exe') --input-target=binary --output-target=elf64-littleaarch64 --binary-architecture=aarch64 (Split-Path $fontSrc -Leaf) $fontObj
    if ($LASTEXITCODE -ne 0) { throw 'objcopy failed: NotoSansCJKsc-Regular.otf' }
} finally { Pop-Location }
$objects += $fontObj

$elf = Join-Path $build 'deep-harness-switch.elf'
$link = @(
    '-T',(Join-Path $repo 'linker\switch-lld.ld'),'-pie','--no-dynamic-linker','--gc-sections',
    '-z','text','-z','now','--build-id=sha1','-u','main',
    (Join-Path $build 'switch_crt0.o'),(Join-Path $gccPic 'crti.o'),(Join-Path $gccPic 'crtbegin.o'),
    $objects,
    '--start-group',
    (Join-Path $portlib 'libSDL2_ttf.a'),
    (Join-Path $portlib 'libSDL2.a'),
    (Join-Path $portlib 'libfreetype.a'),
    (Join-Path $portlib 'libharfbuzz.a'),
    (Join-Path $portlib 'libfribidi.a'),
    (Join-Path $portlib 'libpng16.a'),
    (Join-Path $portlib 'libbz2.a'),
    (Join-Path $portlib 'libz.a'),
    (Join-Path $portlib 'libEGL.a'),
    (Join-Path $portlib 'libGLESv2.a'),
    (Join-Path $portlib 'libglapi.a'),
    (Join-Path $portlib 'libdrm_nouveau.a'),
    (Join-Path $toolRoot 'libnx\lib\libnx.a'),
    (Join-Path $newlibPic 'libc.a'),(Join-Path $newlibPic 'libm.a'),(Join-Path $newlibPic 'libsysbase.a'),
    (Join-Path $newlibPic 'libstdc++.a'),(Join-Path $newlibPic 'libsupc++.a'),(Join-Path $gccPic 'libgcc.a'),
    '--end-group',
    (Join-Path $gccPic 'crtend.o'),(Join-Path $gccPic 'crtn.o'),
    '-o',$elf
)
Write-Host 'linking'
& (Join-Path $llvm 'ld.lld.exe') @link
if ($LASTEXITCODE -ne 0) { throw 'link failed' }

$nacp = Join-Path $build 'deep-harness-switch.nacp'
$nro  = Join-Path $repo 'deep-harness-switch.nro'
& (Join-Path $switchTools 'nacptool.exe') --create 'DEEP HARNESS SWITCH' 'deep-harness-switch' '0.1.0' $nacp
if ($LASTEXITCODE -ne 0) { throw 'nacp failed' }
& (Join-Path $switchTools 'elf2nro.exe') $elf $nro "--nacp=$nacp" "--icon=$(Join-Path $repo 'icon.jpg')" "--romfsdir=$(Join-Path $repo 'romfs')"
if ($LASTEXITCODE -ne 0) { throw 'nro failed' }

Get-Item $nro | Select-Object FullName, Length
