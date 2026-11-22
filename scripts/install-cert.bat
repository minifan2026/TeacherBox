@echo off
chcp 65001 >nul
cd /d "%~dp0"
echo.
echo  把 TeacherBox 的签名证书装进本机的「受信任的发布者」
echo  发布者：minifan
echo.
if not exist TeacherBox.cer (
  echo  找不到 TeacherBox.cer
  pause
  exit /b 1
)
certutil -user -addstore "Root" TeacherBox.cer >nul
certutil -user -addstore "TrustedPublisher" TeacherBox.cer >nul
echo.
echo  完成。重新打开 main.exe，就不会再提示"未知发布者"了。
echo.
pause
