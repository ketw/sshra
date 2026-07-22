@echo off
:: =============================================================================
:: build-msagent.bat - Build msagent.exe (Windows service)
:: Requires: Visual Studio (cl.exe) OR MinGW-w64 (gcc)
:: Run from: project root  OR  build\  directory
:: =============================================================================
setlocal EnableDelayedExpansion

:: Resolve project root (parent of build\)
set "SCRIPT_DIR=%~dp0"
pushd "%SCRIPT_DIR%\.."
set "ROOT=%CD%"
popd

set "SRC=%ROOT%\agent\agent.c"
set "INC=%ROOT%\common"
set "OUT=%ROOT%\build\msagent.exe"

echo.
echo  [*] Building msagent.exe
echo      Source : %SRC%
echo      Output : %OUT%
echo.

:: ── Try MSVC first ────────────────────────────────────────────────────────────
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
            pdh.lib ^
            iphlpapi.lib ^
            powrprof.lib ^
            setupapi.lib ^
        /SUBSYSTEM:CONSOLE ^
        /MANIFEST:EMBED ^
        /MANIFESTUAC:"level='requireAdministrator' uiAccess='false'"
    goto :check_result
)

:: ── Try MinGW gcc ─────────────────────────────────────────────────────────────
where gcc >nul 2>&1
if %ERRORLEVEL% == 0 (
    echo  [*] Compiler: gcc (MinGW)
    gcc "%SRC%" ^
        -I"%INC%" ^
        -O2 -Wall ^
        -D_WIN32_WINNT=0x0601 ^
        -D_CRT_SECURE_NO_WARNINGS ^
        -municode ^
        -o "%OUT%" ^
        -lws2_32 -ladvapi32 -lpdh -liphlpapi -lpowrprof -lsetupapi -lcrypt32
    goto :check_result
)

echo  [!] No compiler found.
echo      Install Visual Studio Build Tools: https://aka.ms/vs/17/release/vs_BuildTools.exe
echo      Or MinGW-w64: https://www.mingw-w64.org/
exit /b 1

:check_result
if exist "%OUT%" (
    echo.
    echo  [OK] Built: %OUT%
    echo.
    :: Embed administrator manifest if using gcc (rc.exe needed for MSVC UAC embed above)
    echo  [*] Done. Copy msagent.exe to target machines and run install.ps1
) else (
    echo.
    echo  [!!] Build FAILED. Check errors above.
    exit /b 1
)
endlocal
