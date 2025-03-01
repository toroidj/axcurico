@echo off
set RETAIL=1
rem *** set value ***
set arcname=axcuri13.zip
set readme=axcurico.txt
set srcname=axcursrc.7z

rem *** main ***
del /q *.7z 2> NUL
del /q *.zip 2> NUL
del /q *.sp* 2> NUL

del /q *.obj 2> NUL
call m64 -DRELEASE

del /q *.obj 2> NUL
call m32 -DRELEASE

del /q *.obj 2> NUL
call marm64 -DRELEASE

rem *** Source Archive ***
if %RETAIL%==0 goto :skipsource

for %%i in (*) do CT %readme% %%i
ppb /c %%u/7-zip32.dll,a %srcname% -hide -mx=9 makefile *.vcproj *.sln *.def *.bat *.c *.cpp *.h *.rc *.rh
CT %readme% %srcname%
:skipsource

for %%i in (*.exe) do tfilesign sp %%i %%i
for %%i in (*.sp*) do tfilesign sp %%i %%i

for %%i in (*.exe) do CT %readme% %%i
for %%i in (*.sp*) do CT %readme% %%i
ppb /c %%u/7-ZIP32.DLL,a -tzip -hide -mx=7 %arcname% %readme% *.sp* *.exe %srcname%
tfilesign s %arcname% %arcname%
CT %readme% %arcname%
del /q %srcname% 2> NUL
