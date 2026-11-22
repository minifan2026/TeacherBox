@echo off
chcp 936 >nul
cd /d "%~dp0"
echo.
echo   把 TeacherBox 的签名证书装进本机的「受信任的发布者」
echo   发布者：minifan
echo.
if not exist "TeacherBox.cer" goto nocer
echo   正在写入当前用户的证书库……
certutil -user -addstore "Root" "TeacherBox.cer" >nul 2>nul
if errorlevel 1 goto fail
certutil -user -addstore "TrustedPublisher" "TeacherBox.cer" >nul 2>nul
if errorlevel 1 goto fail
echo.
echo   完成。重新打开 main.exe，就不会再提示「未知发布者」了。
echo.
pause
exit /b 0

:nocer
echo   找不到 TeacherBox.cer
echo   请把本文件和 TeacherBox.cer 放在同一个文件夹里再运行。
echo.
pause
exit /b 1

:fail
echo.
echo   写入证书库失败。可能的原因：
echo   - 这台电脑的组策略禁止导入证书
echo   - 证书文件损坏或被改过
echo   可以改用管理员身份再运行一次本文件。
echo.
pause
exit /b 2