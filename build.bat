@echo off
REM Builds both 64-bit and 32-bit. Injector and payload MUST match the game's bitness.

where cmake >nul 2>nul
if errorlevel 1 (
    echo cmake not found. Install it, or open this folder in Visual Studio
    echo with the "C++ CMake tools for Windows" component installed.
    exit /b 1
)

echo === x64 ===
cmake -B build64 -A x64 || exit /b 1
cmake --build build64 --config Release || exit /b 1

echo === x86 ===
cmake -B build32 -A Win32 || exit /b 1
cmake --build build32 --config Release || exit /b 1

echo.
echo x64 output: build64\bin\Release
echo x86 output: build32\bin\Release
echo.
echo Use the x64 pair for 64-bit games, the x86 pair for 32-bit games.
echo.
echo For a release zip, rename the x86 pair and put all four files together:
echo     monodump.exe            (x64 injector)
echo     monodump_payload.dll    (x64 payload)
echo     monodump32.exe          (x86 injector, renamed from build32)
echo     monodump_payload32.dll  (x86 payload, renamed from build32)

pause