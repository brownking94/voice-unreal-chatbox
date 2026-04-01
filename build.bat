@echo off
echo START
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"
echo AFTER VCVARS
cmake --version
echo DONE
