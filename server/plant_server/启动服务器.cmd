@echo off
title 植小伴 · 服务器
cd /d "%~dp0"

netstat -ano | findstr ":8011 " | findstr LISTENING >nul
if %errorlevel%==0 (
  echo.
  echo   服务已经在运行，直接用：
  echo     管理台    http://192.168.3.12:8011/admin/    口令 PLANT_ADMIN_PASSWORD
  echo     设备接口  http://192.168.3.12:8011/api/v1/health
  echo.
  pause
  exit /b 0
)

rem 从系统环境变量里取 AI 密钥（只在这台电脑的注册表里，不写进项目文件）
for /f "skip=2 tokens=2,*" %%A in ('reg query "HKCU\Environment" /v PLANT_MIMO_KEY 2^>nul') do set "PLANT_MIMO_KEY=%%B"
set "PLANT_AI_MODE=mimo"
if not defined PLANT_MIMO_KEY echo   [注意] 没找到 AI 密钥环境变量，这次会以演示模式启动。

echo.
echo   正在启动植小伴服务器...
"C:\Python314\python.exe" -m app.services.service_ctl --start --port 8011
ping -n 8 127.0.0.1 >nul

echo.
echo   管理台    http://192.168.3.12:8011/admin/    口令 PLANT_ADMIN_PASSWORD
echo   设备接口  http://192.168.3.12:8011/api/v1/health
echo.
echo   关掉这个窗口不影响服务器，停服务用管理台左下角的「重启服务」。
pause