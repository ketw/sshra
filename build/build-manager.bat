@echo off
:: =============================================================================
:: build-manager.bat - Build kiro-manager.exe (laptop TUI)
:: =============================================================================
setlocal EnableDelayedExpansion

set "SCRIPT_DIR=%~dp0"
pushd "%SCRIPT_DIR%\.."
set "ROOT=%CD%"
popd

set "SRC=%ROOT%\manager\manager.c"
set "INC=%ROOT%\common"
set "OUT=%ROOT%\build\kiro-manager.exe"

echo.
echo  [*] Building kiro-manager.exe
echo      Source : %SRC%
echo      Output : %OUT%
echo.

where cl.exe >nul 2>&1
if %ERRORLEVEL% == 0 (
    echo  [*] Compiler: MSVC cl.exe
    cl.exe "%SRC%" ^
        /Fe:"%OUT%" ^
        /I"%INC%" ^
        /O2 /W3 /WX- ^
        /D_WIN32_WINNT=0x0601 ^
        /D_CRT_SECURE_NO_WARNINGS ^
        /link ^
            ws2_32.lib ^
            advapi32.lib ^
            iphlpapi.lib ^
            shell32.lib ^
        /SUBSYSTEM:CONSOLE
    goto :check
)

where gcc >nul 2>&1
if %ERRORLEVEL% == 0 (
    echo  [*] Compiler: gcc (MinGW)
    gcc "%SRC%" ^
        -I"%INC%" ^
        -O2 -Wall ^
        -D_WIN32_WINNT=0x0601 ^
        -D_CRT_SECURE_NO_WARNINGS ^
        -o "%OUT%" ^
        -lws2_32 -ladvapi32 -liphlpapi -lshell32
    goto :check
)

echo  [!] No compiler found. Install MSVC or MinGW-w64.
exit /b 1

:check
if exist "%OUT%" (
    echo.
    echo  [OK] Built: %OUT%
) else (
    echo.
    echo  [!!] Build FAILED.
    exit /b 1
)
endlocal
