@echo off
rem Builds Depthyum and assembles a ready-to-install zip in dist\.
rem Run from "x64 Native Tools Command Prompt for VS" in the project folder.
rem Optional paths (defaults match the Lensyum setup):
rem   set ORT_DIR=C:\SDK\ort          (extracted Microsoft.ML.OnnxRuntime.DirectML package)
rem   set DML_DIR=C:\SDK\dml          (extracted Microsoft.AI.DirectML package)
rem   set MODEL=C:\SDK\lensyum_depth.onnx   (Depth Anything V2 Small; the file Lensyum already uses works)

setlocal
cd /d "%~dp0"
if "%ORT_DIR%"=="" set ORT_DIR=C:\SDK\ort
if "%DML_DIR%"=="" set DML_DIR=C:\SDK\dml

echo Building...
cmake --build build --target Depthyum
if errorlevel 1 (
    echo.
    echo Build failed. Configure first, for example:
    echo   cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DAE_SDK_DIR="C:/SDK/AfterEffectsSDK"
    exit /b 1
)

set AEX=build\ae\Depthyum.aex
if not exist "%AEX%" set AEX=build\ae\Release\Depthyum.aex
if not exist "%AEX%" (
    echo Depthyum.aex not found in build\ae
    exit /b 1
)

set OUT=dist\Depthyum
if exist dist rmdir /s /q dist
mkdir "%OUT%"
copy /y "%AEX%" "%OUT%\" >nul
copy /y README.md "%OUT%\" >nul
copy /y THIRD_PARTY.md "%OUT%\" >nul
copy /y LICENSE "%OUT%\LICENSE.txt" >nul
copy /y docs\INSTALL.txt "%OUT%\" >nul
mkdir "%OUT%\licenses"
copy /y licenses\*.txt "%OUT%\licenses\" >nul

if exist "%ORT_DIR%\runtimes\win-x64\native\onnxruntime.dll" (
    copy /y "%ORT_DIR%\runtimes\win-x64\native\onnxruntime.dll" "%OUT%\depthyum_ort.dll" >nul
    if exist "%ORT_DIR%\LICENSE" copy /y "%ORT_DIR%\LICENSE" "%OUT%\licenses\ONNXRUNTIME-LICENSE.txt" >nul
    if exist "%ORT_DIR%\ThirdPartyNotices.txt" copy /y "%ORT_DIR%\ThirdPartyNotices.txt" "%OUT%\licenses\ONNXRUNTIME-ThirdPartyNotices.txt" >nul
    if exist "%ORT_DIR%\runtimes\win-x64\native\onnxruntime_providers_shared.dll" copy /y "%ORT_DIR%\runtimes\win-x64\native\onnxruntime_providers_shared.dll" "%OUT%\" >nul
) else (
    echo Warning: ONNX Runtime not found in %ORT_DIR%, the effect will show a red frame.
)
if exist "%DML_DIR%\bin\x64-win\DirectML.dll" (
    copy /y "%DML_DIR%\bin\x64-win\DirectML.dll" "%OUT%\" >nul
    for %%F in ("%DML_DIR%\LICENSE*" "%DML_DIR%\ThirdPartyNotices*") do if exist "%%~F" copy /y "%%~F" "%OUT%\licenses\DirectML-%%~nF.txt" >nul
) else if exist "%ORT_DIR%\runtimes\win-x64\native\DirectML.dll" (
    copy /y "%ORT_DIR%\runtimes\win-x64\native\DirectML.dll" "%OUT%\" >nul
) else (
    echo Warning: DirectML.dll not found, the estimate will run on the CPU.
)

rem The model: MODEL, then the file Lensyum uses, then a Lensyum install.
if not "%MODEL%"=="" if exist "%MODEL%" copy /y "%MODEL%" "%OUT%\depthyum_depth.onnx" >nul
if not exist "%OUT%\depthyum_depth.onnx" if exist "C:\SDK\lensyum_depth.onnx" copy /y "C:\SDK\lensyum_depth.onnx" "%OUT%\depthyum_depth.onnx" >nul
if not exist "%OUT%\depthyum_depth.onnx" if exist "C:\Program Files\Adobe\Common\Plug-ins\7.0\MediaCore\Lensyum\lensyum_depth.onnx" copy /y "C:\Program Files\Adobe\Common\Plug-ins\7.0\MediaCore\Lensyum\lensyum_depth.onnx" "%OUT%\depthyum_depth.onnx" >nul
if not exist "%OUT%\depthyum_depth.onnx" echo Warning: depth model not found, set MODEL=path to include it.

if not exist "%OUT%\licenses\DirectML-*" echo Warning: no DirectML license file found in %DML_DIR%; copy it by hand to %OUT%\licenses\.
powershell -NoProfile -Command "Compress-Archive -Path 'dist\Depthyum' -DestinationPath 'dist\Depthyum-1.0-win64.zip' -Force"
if errorlevel 1 exit /b 1
echo.
echo Done: dist\Depthyum-1.0-win64.zip
dir dist\Depthyum
endlocal
