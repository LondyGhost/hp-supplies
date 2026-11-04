# hp-supplies Windows 32 位构建（目标: Windows 7 SP1 及以上, 32/64 位通吃）
#
# 产物: out/windows-i686/hp-supplies.exe  单文件: 双击=GUI(自动隐藏临时
#       控制台), 终端带参数=CLI; dist/hp-supplies-<ver>-windows-i686.zip
#
# 关键点:
#   - 工具链为 i686 mingw-builds (msvcrt 运行库): Win7/8/8.1 免装 UCRT
#   - GUI 为原生 Win32 实现(src/gui-win32.h), 仅依赖 libusb(静态链接)
#   - -D_WIN32_WINNT=0x0601 锁定 API 面为 Windows 7
#   - 默认工具链位置 D:\workbuddy\toolchains\, 可用参数覆盖
#
# 用法: powershell -File packaging/windows/build-win7-i686.ps1 [-Version 1.2.1]
param(
    [string]$Version      = "1.2.1",
    [string]$ToolchainDir = "D:\workbuddy\toolchains\mingw32\bin",
    [string]$LibusbDir    = "D:\workbuddy\toolchains\libusb-1.0.30"
)
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path

$gcc    = Join-Path $ToolchainDir 'gcc.exe'
$windres = Join-Path $ToolchainDir 'windres.exe'
foreach ($t in $gcc, $windres) {
    if (-not (Test-Path $t)) { throw "找不到工具: $t (用 -ToolchainDir 指定)" }
}
if (-not (Test-Path (Join-Path $LibusbDir 'include'))) {
    throw "找不到 libusb 开发文件: $LibusbDir (用 -LibusbDir 指定)"
}

# main.c 写的是 #include <libusb-1.0/libusb.h>; libusb 1.0.30+ 官方包头文件
# 直接放在 include\ 下, 补建 include\libusb-1.0\ 使两种布局都能命中
$inc = Join-Path $LibusbDir 'include'
if (-not (Test-Path (Join-Path $inc 'libusb-1.0\libusb.h')) -and
        (Test-Path (Join-Path $inc 'libusb.h'))) {
    New-Item -ItemType Directory -Force -Path (Join-Path $inc 'libusb-1.0') | Out-Null
    Copy-Item (Join-Path $inc 'libusb.h') (Join-Path $inc 'libusb-1.0\libusb.h')
}

$out = Join-Path $repo 'out\windows-i686'
New-Item -ItemType Directory -Force -Path $out | Out-Null

# 工具链 bin 必须在 PATH 最前: gcc 以完整路径调用 i686-w64-mingw32\bin\as.exe,
# 该目录下无运行库 DLL, 否则会顺着 PATH 加载到其它工具链(如 ProgramData\mingw64)
# 的 64 位同名 DLL, 子进程以 0xC000007B 静默死亡
$env:PATH = "$ToolchainDir;$env:PATH"

$cflags = @(
    '-O2', '-Wall', '-Wno-deprecated-declarations',
    ('-DAPP_VERSION="' + $Version + '"'),
    '-D_WIN32_WINNT=0x0601', '-DWINVER=0x0601',
    ('-I' + (Join-Path $LibusbDir 'include'))
)
$libs = @(
    # 直接指定静态库文件: MinGW32\static 里同时有 .a 与 .dll.a,
    # 用 -L/-l 会被 ld 优先解析到 .dll.a (DLL 导入库)
    (Join-Path $LibusbDir 'MinGW32\static\libusb-1.0.a'),
    '-lws2_32', '-lsetupapi', '-ladvapi32', '-lcfgmgr32',
    '-lcomctl32', '-lgdi32', '-luser32',
    '-static-libgcc'
)

"=== [1/2] 资源 (图标 + manifest + versioninfo) ==="
$res = Join-Path $out 'hp-supplies.res'
& $windres (Join-Path $PSScriptRoot 'hp-supplies.rc') -I $PSScriptRoot -O coff -o $res
if ($LASTEXITCODE -ne 0) { throw "windres 失败" }

$src = Join-Path $repo 'src\main.c'
"=== [2/2] hp-supplies.exe (单文件: 双击=GUI, 命令行=CLI) ==="
& $gcc @cflags $res $src @libs -o (Join-Path $out 'hp-supplies.exe')
if ($LASTEXITCODE -ne 0) { throw "gcc 失败" }

Get-ChildItem $out | Select-Object Name, Length | Format-Table -AutoSize | Out-String
"构建完成: $out"
