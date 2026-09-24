@echo off
windres app.rc -O coff -o app_res.o || goto :err
x86_64-w64-mingw32-gcc -O2 -mwindows -o MiSideload.exe mi_sideload_gui.c app_res.o ^
  -lsetupapi -lwinusb -lwinhttp -lbcrypt -lcrypt32 -lcomctl32 -lcomdlg32 ^
  -lgdi32 -luser32 -ladvapi32 -lshell32 -static -static-libgcc || goto :err
echo Built MiSideload.exe & goto :eof
:err
echo Build failed. & exit /b 1
