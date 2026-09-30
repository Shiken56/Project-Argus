@echo off
set CUBEPROG_DIR=C:\Program Files\STMicroelectronics\STM32Cube\STM32CubeProgrammer\bin
set PROG_CLI="%CUBEPROG_DIR%\STM32_Programmer_CLI.exe"
set EXT_LOADER="%CUBEPROG_DIR%\ExternalLoader\MX66UW1G45G_STM32N6570-DK.stldr"

if not exist %PROG_CLI% (
    echo Error: STM32_Programmer_CLI.exe not found!
    pause
    exit /b 1
)

echo ===================================================
echo 1. Signing Binaries...
echo ===================================================
call sign_binaries.bat
if %ERRORLEVEL% neq 0 (
    echo Signing failed!
    pause
    exit /b %ERRORLEVEL%
)

echo.
echo ===================================================
echo 2. Flashing FSBL and Appli to External Flash...
echo    FSBL at 0x70000000
echo    Appli at 0x70100000
echo ===================================================

%PROG_CLI% -c port=SWD mode=HOTPLUG -el %EXT_LOADER% -d "FSBL\Debug\mtk3bsp2_stm32n657_FSBL-trusted.bin" 0x70000000 -v -d "Appli\Debug\mtk3bsp2_stm32n657_Appli-trusted.bin" 0x70100000 -v -rst

if %ERRORLEVEL% equ 0 (
    echo.
    echo ===================================================
    echo Flashing and Verification SUCCESSFUL!
    echo Ensure boot switch is set to Boot from External Flash and reset the board.
    echo ===================================================
) else (
    echo.
    echo Flashing failed with error code %ERRORLEVEL%.
)
