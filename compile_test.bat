@echo off
call "D:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
cl.exe /W3 /Zi /O2 /D_AMD64_ /DWIN64 /I.\inc /I"D:\Program Files (x86)\Windows Kits\10\Include\10.0.26100.0\um" /I"D:\Program Files (x86)\Windows Kits\10\Include\10.0.26100.0\shared" /I"D:\Program Files (x86)\Windows Kits\10\Include\10.0.26100.0\ucrt" .\src\test\test-psp-driver.c /Fe.\output\test-psp-driver.exe /link /LIBPATH:"D:\Program Files (x86)\Windows Kits\10\Lib\10.0.26100.0\um\x64" /LIBPATH:"D:\Program Files (x86)\Windows Kits\10\Lib\10.0.26100.0\ucrt\x64" advapi32.lib
