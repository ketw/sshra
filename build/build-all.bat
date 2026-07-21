@echo off
:: =============================================================================
:: build-all.bat - Build everything (agent + manager) on Windows
:: =============================================================================
echo.
echo  ============================================
echo   KiroAccess - Build All
echo  ============================================
echo.

call "%~dp0build-agent.bat"
if %ERRORLEVEL% neq 0 exit /b %ERRORLEVEL%

call "%~dp0build-manager.bat"
if %ERRORLEVEL% neq 0 exit /b %ERRORLEVEL%

echo.
echo  ============================================
echo   All builds complete.
echo.
echo   Artifacts:
echo     build\kiro-agent.exe   <- deploy to each PC
echo     build\kiro-manager.exe <- run on your laptop
echo  ============================================
echo.
