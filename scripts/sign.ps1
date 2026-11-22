# 给 dist 里的三个 exe 做代码签名。
#
#   .\scripts\sign.ps1               # 证书不存在就自签一张，然后签 dist\*.exe
#   .\scripts\sign.ps1 -NewCert      # 强制重签一张新证书
#
# 自签证书只解决"有没有签名"的问题：装的人不把根证书导入"受信任的发布者"，
# 打开时仍会提示未知发布者。真正让普通用户无感，得买 OV/EV 代码签名证书。

param(
  [switch]$NewCert,
  [string]$Subject = "CN=minifan, O=minifan"
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$dist = Join-Path $root 'dist'
$certDir = Join-Path $root 'certs'

function Find-SignTool {
  $kits = Get-ChildItem "${env:ProgramFiles(x86)}\Windows Kits\10\bin" -Directory -ErrorAction SilentlyContinue |
          Sort-Object Name -Descending
  foreach ($k in $kits) {
    $p = Join-Path $k.FullName 'x64\signtool.exe'
    if (Test-Path $p) { return $p }
  }
  return $null
}

$signtool = Find-SignTool
if (-not $signtool) { throw '找不到 signtool.exe（装 Windows SDK 后才有）' }

$cert = Get-ChildItem Cert:\CurrentUser\My -CodeSigningCert |
        Where-Object { $_.Subject -eq $Subject } |
        Sort-Object NotAfter -Descending | Select-Object -First 1

if ($NewCert -or -not $cert) {
  Write-Host "生成自签代码签名证书：$Subject"
  $cert = New-SelfSignedCertificate `
    -Type CodeSigningCert `
    -Subject $Subject `
    -KeyAlgorithm RSA -KeyLength 2048 `
    -HashAlgorithm SHA256 `
    -KeyExportPolicy Exportable `
    -CertStoreLocation 'Cert:\CurrentUser\My' `
    -NotAfter (Get-Date).AddYears(3)
  New-Item -ItemType Directory -Force $certDir | Out-Null
  $pw = ConvertTo-SecureString -String 'teacherbox-dev' -Force -AsPlainText
  Export-PfxCertificate -Cert $cert -FilePath (Join-Path $certDir 'TeacherBox.pfx') -Password $pw | Out-Null
  Export-Certificate -Cert $cert -FilePath (Join-Path $certDir 'TeacherBox.cer') | Out-Null
  Write-Host "证书已导出到 certs\（pfx 带私钥，别外传）" -ForegroundColor Yellow
}

$exes = Get-ChildItem (Join-Path $dist '*.exe')
if (-not $exes) { throw "dist 里没有 exe，先跑 build.ps1" }

foreach ($e in $exes) {
  & $signtool sign /sha1 $cert.Thumbprint /fd sha256 /td sha256 `
    /tr http://timestamp.digicert.com /v $e.FullName *> $null
  if ($LASTEXITCODE -ne 0) {
    # 时间戳服务器连不上就签个不带时间戳的，证书到期前一直有效
    Write-Host "  $($e.Name)：时间戳失败，改为不带时间戳签名" -ForegroundColor Yellow
    & $signtool sign /sha1 $cert.Thumbprint /fd sha256 $e.FullName *> $null
    if ($LASTEXITCODE -ne 0) { throw "签名失败：$($e.Name)" }
  }
  Write-Host "  已签名 $($e.Name)" -ForegroundColor Green
}

Write-Host ''
Get-ChildItem (Join-Path $dist '*.exe') | ForEach-Object {
  $sig = Get-AuthenticodeSignature $_.FullName
  $tip = if ($sig.Status -eq 'Valid') { '受信任' } else { '已签名，但本机没信任这张自签证书' }
  Write-Host ("  {0,-11} {1}（{2}）" -f $_.Name, $sig.Status, $tip)
}
Write-Host ''
Write-Host '让别的机器认识这张证书：双击 certs\TeacherBox.cer → 安装证书 → 放入「受信任的发布者」。' -ForegroundColor Yellow
