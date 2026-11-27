# TeacherBox 一键构建脚本（Windows）
#
#   .\build.ps1                 # 默认 MSVC x64 Release，构建并装配 dist
#   .\build.ps1 -Config Debug
#   .\build.ps1 -Gcc             # 用 MinGW（gcc + Ninja/MinGW Makefiles），需要 gcc 在 PATH
#   .\build.ps1 -SkipAssemble    # 只编译

param(
  [string]$Config = "Release",
  [switch]$Gcc,
  [switch]$SkipAssemble,
  [switch]$Clean
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$buildDir = Join-Path $root "build"
$distDir = Join-Path $root "dist"
$sdkHeader = Join-Path $root "_deps\webview2\build\native\include\WebView2.h"

Write-Host "== TeacherBox 构建 ==" -ForegroundColor Cyan
Write-Host "工程目录: $root"

# 1. 依赖
if (-not (Test-Path $sdkHeader)) {
  Write-Host "缺少 WebView2 SDK，开始自动获取…" -ForegroundColor Yellow
  & (Join-Path $root "scripts\fetch_webview2.ps1")
}
if (-not (Test-Path (Join-Path $root "src\gui\webview2_iids.h"))) {
  & (Join-Path $root "scripts\gen_iids.ps1")
}

if ($Clean -and (Test-Path $buildDir)) {
  Write-Host "清理构建目录…"
  Remove-Item -LiteralPath $buildDir -Recurse -Force
}

# 2. 选择工具链
if ($Gcc) {
  $gcc = Get-Command gcc -ErrorAction SilentlyContinue
  if (-not $gcc) { throw "找不到 gcc，请先安装 MinGW-w64 并加入 PATH（或去掉 -Gcc 用 MSVC）" }
  $ninja = Get-Command ninja -ErrorAction SilentlyContinue
  $gen = if ($ninja) { "Ninja" } else { "MinGW Makefiles" }
  Write-Host "使用 GCC: $($gcc.Source)  生成器: $gen" -ForegroundColor Green
  & cmake -S $root -B $buildDir -G $gen -DCMAKE_BUILD_TYPE=$Config
  if ($LASTEXITCODE -ne 0) { throw "CMake 配置失败" }
  & cmake --build $buildDir --parallel
} else {
  $gen = "Visual Studio 17 2022"
  Write-Host "使用 MSVC ($gen / x64)…" -ForegroundColor Green
  & cmake -S $root -B $buildDir -G $gen -A x64
  if ($LASTEXITCODE -ne 0) { throw "CMake 配置失败" }
  & cmake --build $buildDir --config $Config --parallel
}
if ($LASTEXITCODE -ne 0) { throw "编译失败" }

$binDir = Join-Path $buildDir "bin"
if (-not (Test-Path (Join-Path $binDir "main.exe"))) {
  # 部分生成器会把产物放在 bin\<Config>
  if (Test-Path (Join-Path $binDir "$Config\main.exe")) { $binDir = Join-Path $binDir $Config }
  else { throw "找不到编译产物 main.exe（检查 $binDir）" }
}

if ($SkipAssemble) {
  Write-Host "编译完成，产物: $binDir" -ForegroundColor Green
  exit 0
}

# 3. 装配 dist
Write-Host "装配 dist…" -ForegroundColor Cyan
if (Test-Path $distDir) {
  try {
    Remove-Item -LiteralPath $distDir -Recurse -Force -ErrorAction Stop
  } catch {
    # 工具目录下如果还挂着 shell / python 之类的进程，目录会被占住，单个清掉，
    # 剩下的交给下面的复制覆盖，不让整个构建失败。
    Write-Host "  dist 有文件被占用，逐个清理…" -ForegroundColor DarkYellow
    Get-ChildItem -LiteralPath $distDir -Force | ForEach-Object {
      try { Remove-Item -LiteralPath $_.FullName -Recurse -Force -ErrorAction Stop }
      catch { Write-Host "  跳过 $($_.Name)" -ForegroundColor DarkYellow }
    }
  }
}
New-Item -ItemType Directory -Force $distDir | Out-Null

foreach ($exe in "main.exe", "update.exe", "gui.exe") {
  Copy-Item -LiteralPath (Join-Path $binDir $exe) -Destination $distDir
}
Copy-Item -LiteralPath (Join-Path $buildDir "bin\WebView2Loader.dll") -Destination $distDir -ErrorAction SilentlyContinue
if (-not (Test-Path (Join-Path $distDir "WebView2Loader.dll"))) {
  Copy-Item -LiteralPath (Join-Path $root "_deps\webview2\build\native\x64\WebView2Loader.dll") -Destination $distDir
}

Copy-Item -LiteralPath (Join-Path $root "web") -Destination $distDir -Recurse
Copy-Item -LiteralPath (Join-Path $root "tools") -Destination $distDir -Recurse
Copy-Item -LiteralPath (Join-Path $root "update.json") -Destination $distDir
# 自签证书的公开部分一起发出去，装机的人双击导入「受信任的发布者」就不会再提示未知发布者
$cer = Join-Path $root "certs\TeacherBox.cer"
if (Test-Path $cer) {
  Copy-Item -LiteralPath $cer -Destination $distDir
  # 一键把证书装进「受信任的发布者」，装完就不再提示未知发布者
  $bat = Join-Path $root "scripts\install-cert.bat"
  if (Test-Path $bat) { Copy-Item -LiteralPath $bat -Destination (Join-Path $distDir "安装证书.bat") }
}
Copy-Item -LiteralPath (Join-Path $root "README.md") -Destination $distDir -ErrorAction SilentlyContinue

# 3.5 内置运行库：别人机器上没装 VC++ 运行库 / MinGW 运行库时，不至于一开就报「找不到 xxx.dll」
#  - dist 根：VC++ x64 CRT（gui/update/main 自己要用，exe 同目录优先加载）
#  - dist\runtime：MinGW 的 libgcc/libstdc++/winpthread（工具子进程沿 PATH 找到，见 util.cpp）
Write-Host "收集运行库 DLL…" -ForegroundColor Cyan
$vcCrt = Get-ChildItem "${env:ProgramFiles}\Microsoft Visual Studio\2022" -Recurse -Directory `
          -Filter "Microsoft.VC14*.CRT" -ErrorAction SilentlyContinue |
         Where-Object { $_.FullName -match '\\x64\\' } |
         Sort-Object FullName -Descending | Select-Object -First 1
if ($vcCrt) {
  Copy-Item -Path (Join-Path $vcCrt.FullName "*.dll") -Destination $distDir -Force
  Write-Host "  VC++ CRT: $($vcCrt.FullName)" -ForegroundColor DarkGray
} else {
  Write-Host "  没找到 VC++ 可再发行 CRT（装 Visual Studio 的「C++ 桌面开发」才有）" -ForegroundColor Yellow
}

$rtDir = Join-Path $distDir "runtime"
New-Item -ItemType Directory -Force $rtDir | Out-Null
$mingwDirs = @()
$gcc = Get-Command gcc -ErrorAction SilentlyContinue
if ($gcc) { $mingwDirs += Split-Path $gcc.Source -Parent }
foreach ($cand in "C:\Program Files\Git\ucrt64\bin", "C:\msys64\ucrt64\bin", "C:\mingw64\bin") {
  if (Test-Path $cand) { $mingwDirs += $cand }
}
foreach ($d in $mingwDirs) {
  foreach ($f in "libgcc_s_seh-1.dll", "libstdc++-6.dll", "libwinpthread-1.dll", "libatomic-1.dll") {
    $p = Join-Path $d $f
    if ((Test-Path $p) -and -not (Test-Path (Join-Path $rtDir $f))) {
      Copy-Item -LiteralPath $p -Destination $rtDir -Force
    }
  }
}
if (-not (Get-ChildItem $rtDir -File -ErrorAction SilentlyContinue)) {
  Remove-Item -LiteralPath $rtDir -Force -ErrorAction SilentlyContinue
  Write-Host "  runtime：本机没有 MinGW 运行库，跳过" -ForegroundColor DarkGray
} else {
  Write-Host "  runtime: $($rtDir)" -ForegroundColor DarkGray
}

Write-Host ""
Write-Host "构建完成 ✔  $distDir" -ForegroundColor Green
Get-ChildItem -LiteralPath $distDir | Select-Object Name, Length | Format-Table -AutoSize

