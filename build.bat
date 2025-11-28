@echo off
set NASM_PATH=N:\Nein_\NASM\
set METAMOD=N:\Nein_\KTP Git Projects\metamod-am
set HLSDK=N:\Nein_\KTP Git Projects\hlsdk
"N:\Nein_\Visual Studio 2022\MSBuild\Current\Bin\MSBuild.exe" "N:\Nein_\KTP Git Projects\KTPAMXX\amxmodx\msvc12\amxmodx_mm.vcxproj" /p:Configuration=JITRelease /p:Platform=Win32 /p:PlatformToolset=v143 /t:Rebuild /m /v:minimal
