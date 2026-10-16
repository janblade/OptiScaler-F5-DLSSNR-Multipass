@echo off
rem Run from a Visual Studio 2022 x64 Native Tools command prompt.
setlocal
pushd "%~dp0.."
if not exist build\improved-f5low mkdir build\improved-f5low
set "OUT=build\improved-f5low"
cl /nologo /std:c++20 /EHsc /O2 /Fe:%OUT%\nr-submission.exe tests\nr_external_submission.cpp
if errorlevel 1 goto failed
%OUT%\nr-submission.exe
if errorlevel 1 goto failed
cl /nologo /std:c++20 /EHsc /O2 /Fe:%OUT%\nr-bytecode.exe tests\nr_shader_bytecode_smoke.cpp d3dcompiler.lib
if errorlevel 1 goto failed
%OUT%\nr-bytecode.exe
if errorlevel 1 goto failed
cl /nologo /std:c++20 /EHsc /O2 /Fe:%OUT%\nr-motion-debug.exe tests\nr_motion_debug_gpu.cpp OptiScaler\motion\OpticalFlow_Dx12.cpp OptiScaler\motion\SceneCut_Dx12.cpp d3d12.lib dxgi.lib d3dcompiler.lib dxguid.lib
if errorlevel 1 goto failed
%OUT%\nr-motion-debug.exe 641 361
if errorlevel 1 goto failed
%OUT%\nr-motion-debug.exe 1920 1080
if errorlevel 1 goto failed
cl /nologo /std:c++20 /EHsc /O2 /Iexternal\vulkan\include /IOptiScaler\include /Fe:%OUT%\nr-debug-interop.exe tests\nr_debug_preview_interop_gpu.cpp OptiScaler\native\SharedFrame.cpp OptiScaler\native\SharedFrameVk.cpp d3d11.lib d3d12.lib dxgi.lib vulkan-1.lib /link /LIBPATH:OptiScaler\library\vulkan
if errorlevel 1 goto failed
%OUT%\nr-debug-interop.exe
if errorlevel 1 goto failed
cl /nologo /std:c++20 /EHsc /O2 /Fe:%OUT%\nr-flow.exe tests\nr_optical_flow_gpu.cpp OptiScaler\motion\OpticalFlow_Dx12.cpp OptiScaler\motion\SceneCut_Dx12.cpp d3d12.lib dxgi.lib d3dcompiler.lib
if errorlevel 1 goto failed
%OUT%\nr-flow.exe
if errorlevel 1 goto failed
cl /nologo /std:c++20 /EHsc /O2 /Fe:%OUT%\nr-producer.exe tests\nr_native_producer_gpu.cpp OptiScaler\native\NativeProducer.cpp OptiScaler\motion\OpticalFlow_Dx12.cpp OptiScaler\motion\SceneCut_Dx12.cpp OptiScaler\motion\TrustMask_Dx12.cpp d3d12.lib dxgi.lib d3dcompiler.lib user32.lib
if errorlevel 1 goto failed
%OUT%\nr-producer.exe
if errorlevel 1 goto failed
echo Improved F5Low tests passed.
popd
exit /b 0
:failed
popd
exit /b 1
