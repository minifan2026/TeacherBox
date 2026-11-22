# 下载并解包 Microsoft.Web.WebView2 SDK（仅 Windows）
# 结果放在 _deps/webview2，供 CMake 编译时使用。

param(
  [string]$Version = "1.0.2903.40"
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$deps = Join-Path $root "_deps"
$zip  = Join-Path $deps "webview2.zip"
$dst  = Join-Path $deps "webview2"
$hdr  = Join-Path $dst "build\native\include\WebView2.h"

New-Item -ItemType Directory -Force $deps | Out-Null

if (Test-Path $hdr) {
  Write-Host "WebView2 SDK 已存在：$dst" -ForegroundColor Green
} else {
  $url = "https://www.nuget.org/api/v2/package/Microsoft.Web.WebView2/$Version"
  Write-Host "下载 WebView2 SDK $Version …"
  & curl.exe -L --fail --retry 3 --max-time 300 -o $zip $url
  if ($LASTEXITCODE -ne 0) {
    throw "下载失败，请检查网络（或手动下载 NuGet 包解压到 $dst）"
  }
  if (Test-Path $dst) { Remove-Item -LiteralPath $dst -Recurse -Force }
  Expand-Archive -LiteralPath $zip -DestinationPath $dst -Force
  Write-Host "已解包到 $dst" -ForegroundColor Green
}

# 生成接口 IID 常量头文件（避免依赖 __uuidof，MinGW 也能编）
& (Join-Path $PSScriptRoot "gen_iids.ps1")

