@echo off
setlocal enabledelayedexpansion

echo ============================================
echo   EdgeScale Windows Build
echo ============================================

where cmake >nul 2>&1
if %errorlevel% neq 0 (
    echo [ERROR] cmake not found in PATH. Please install CMake.
    exit /b 1
)

where glslc >nul 2>&1
if %errorlevel% neq 0 (
    if defined VULKAN_SDK (
        set "PATH=%VULKAN_SDK%\Bin;%VULKAN_SDK%\bin;%PATH%"
    ) else (
        echo [ERROR] glslc not found. Please install the Vulkan SDK.
        exit /b 1
    )
)

set "BUILD_DIR=build-windows"
if not exist "%BUILD_DIR%" mkdir "%BUILD_DIR%"

echo [1/3] Configuring with CMake...
cmake -B "%BUILD_DIR%" -S . -DCMAKE_BUILD_TYPE=Release -DXFSR_BUILD_DEMO=OFF -DXFSR_BUILD_BENCH=OFF
if %errorlevel% neq 0 (
    echo [ERROR] CMake configuration failed.
    exit /b %errorlevel%
)

echo [2/3] Building EdgeScale...
cmake --build "%BUILD_DIR%" --config Release --target VkLayer_XFSR
if %errorlevel% neq 0 (
    echo [ERROR] Build failed.
    exit /b %errorlevel%
)

echo [3/3] Creating Windows distribution package...
set "DIST_DIR=dist\edgescale-windows-x86_64"
if exist "%DIST_DIR%" rd /s /q "%DIST_DIR%"
mkdir "%DIST_DIR%"
mkdir "%DIST_DIR%\shaders"

if exist "%BUILD_DIR%\VkLayer_XFSR.dll" copy /y "%BUILD_DIR%\VkLayer_XFSR.dll" "%DIST_DIR%\" >nul
if exist "%BUILD_DIR%\Release\VkLayer_XFSR.dll" copy /y "%BUILD_DIR%\Release\VkLayer_XFSR.dll" "%DIST_DIR%\" >nul
if exist "%BUILD_DIR%\bin\Release\VkLayer_XFSR.dll" copy /y "%BUILD_DIR%\bin\Release\VkLayer_XFSR.dll" "%DIST_DIR%\" >nul
if exist "%BUILD_DIR%\bin\VkLayer_XFSR.dll" copy /y "%BUILD_DIR%\bin\VkLayer_XFSR.dll" "%DIST_DIR%\" >nul

if exist "%BUILD_DIR%\VkLayer_EdgeScale.json" copy /y "%BUILD_DIR%\VkLayer_EdgeScale.json" "%DIST_DIR%\" >nul
if exist "%BUILD_DIR%\Release\VkLayer_EdgeScale.json" copy /y "%BUILD_DIR%\Release\VkLayer_EdgeScale.json" "%DIST_DIR%\" >nul

if exist "%BUILD_DIR%\VkLayer_XFSR.json" copy /y "%BUILD_DIR%\VkLayer_XFSR.json" "%DIST_DIR%\" >nul
if exist "%BUILD_DIR%\Release\VkLayer_XFSR.json" copy /y "%BUILD_DIR%\Release\VkLayer_XFSR.json" "%DIST_DIR%\" >nul

if exist "%BUILD_DIR%\shaders\*.spv" copy /y "%BUILD_DIR%\shaders\*.spv" "%DIST_DIR%\shaders\" >nul
copy /y "scripts\install-windows.bat" "%DIST_DIR%\" >nul
copy /y "scripts\uninstall-windows.bat" "%DIST_DIR%\" >nul
copy /y "README.md" "%DIST_DIR%\" >nul

if not exist "%DIST_DIR%\VkLayer_XFSR.dll" (
    echo [ERROR] VkLayer_XFSR.dll was not found after build!
    exit /b 1
)

echo ============================================
echo   Build completed successfully!
echo   Output in: %DIST_DIR%
echo ============================================

