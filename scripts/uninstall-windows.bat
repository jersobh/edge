@echo off
setlocal

set "INSTALL_DIR=%LOCALAPPDATA%\EdgeScale"
set "MANIFEST=%INSTALL_DIR%\VkLayer_EdgeScale.json"
set "LEGACY_MANIFEST=%INSTALL_DIR%\VkLayer_XFSR.json"

echo Removing EdgeScale from Windows Registry...
reg delete "HKCU\Software\Khronos\Vulkan\ImplicitLayers" /v "%MANIFEST%" /f >nul 2>&1
reg delete "HKCU\Software\Khronos\Vulkan\ImplicitLayers" /v "%LEGACY_MANIFEST%" /f >nul 2>&1

if exist "%INSTALL_DIR%" (
    echo Removing %INSTALL_DIR%...
    rd /s /q "%INSTALL_DIR%"
)

echo EdgeScale uninstalled.

