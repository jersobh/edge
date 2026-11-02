@echo off
setlocal

echo ============================================
echo   EdgeScale Windows Layer Installer
echo ============================================

set "INSTALL_DIR=%LOCALAPPDATA%\EdgeScale"
echo Installing EdgeScale to: %INSTALL_DIR%

if not exist "%INSTALL_DIR%" mkdir "%INSTALL_DIR%"
if not exist "%INSTALL_DIR%\shaders" mkdir "%INSTALL_DIR%\shaders"

copy /y "%~dp0VkLayer_XFSR.dll" "%INSTALL_DIR%\" >nul
copy /y "%~dp0VkLayer_EdgeScale.json" "%INSTALL_DIR%\" >nul
copy /y "%~dp0VkLayer_XFSR.json" "%INSTALL_DIR%\" >nul
copy /y "%~dp0shaders\*.spv" "%INSTALL_DIR%\shaders\" >nul

set "MANIFEST=%INSTALL_DIR%\VkLayer_EdgeScale.json"
set "LEGACY_MANIFEST=%INSTALL_DIR%\VkLayer_XFSR.json"

echo Registering layer in Windows Registry (HKCU\Software\Khronos\Vulkan\ImplicitLayers)...
reg add "HKCU\Software\Khronos\Vulkan\ImplicitLayers" /v "%MANIFEST%" /t REG_DWORD /d 0 /f >nul
if %errorlevel% neq 0 (
    echo [ERROR] Failed to register layer in registry.
    exit /b %errorlevel%
)
reg add "HKCU\Software\Khronos\Vulkan\ImplicitLayers" /v "%LEGACY_MANIFEST%" /t REG_DWORD /d 0 /f >nul

echo.
echo ============================================
echo   EdgeScale installed successfully!
echo.
echo   To enable for any game, set environment variable:
echo     ENABLE_EDGESCALE=1  (or ENABLE_XFSR=1)
echo.
echo   Hotkeys (in-game):
echo     F1  : Cycle presets (Quality, Balanced, Performance, Ultra)
echo     F2  : Toggle RCAS or Edge mode (Quality / Perf / Nearest pixel art)
echo     F3  : Cycle upscaler (fsr1, blit, nearest, nis, edge)
echo     F10 : Pin/unpin HUD overlay
echo ============================================

