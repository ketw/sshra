@echo off
:: =============================================================================
:: build-all.bat - Build everything (agent + manager) on Windows
:: =============================================================================
echo.
echo  ============================================
echo   Mass - Build All
echo  ============================================
echo.

call "%~dp0build-msagent.bat"
if %ERRORLEVEL% neq 0 exit /b %ERRORLEVEL%

call "%~dp0build-msmgr.bat"
if %ERRORLEVEL% neq 0 exit /b %ERRORLEVEL%

echo.
echo  ============================================
echo   All builds complete.
echo.
echo   Artifacts:
echo     build\msagent.exe   <- deploy to each PC
echo     build\msmgr.exe <- run on your laptop
echo  ============================================
echo.
