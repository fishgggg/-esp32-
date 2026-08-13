@echo off
set MSYSTEM=
if not defined IDF_PYTHON_ENV_PATH set "IDF_PYTHON_ENV_PATH=D:\Espressif\python_env\idf5.5_py3.11_env"
if not defined IDF_TOOLS_PATH set "IDF_TOOLS_PATH=D:\Espressif"
set "PATH=%IDF_PYTHON_ENV_PATH%\Scripts;%IDF_TOOLS_PATH%\tools\idf-git\2.44.0\cmd;%PATH%"
cd /d "%~dp0build"

rem 用法: flash_s7.bat [COM口]，默认 COM6
set "PORT=%1"
if "%PORT%"=="" set "PORT=COM6"

echo === Flashing SmartHome S7 to %PORT% ===
echo.
echo [1/3] Bootloader...
python -m esptool --chip esp32 -p %PORT% -b 460800 --before default_reset --after hard_reset write_flash --flash_mode dio --flash_freq 40m --flash_size 2MB 0x1000 bootloader\bootloader.bin
if errorlevel 1 goto :error
echo [2/3] Partition table...
python -m esptool --chip esp32 -p %PORT% -b 460800 --before default_reset --after hard_reset write_flash --flash_mode dio --flash_freq 40m --flash_size 2MB 0x8000 partition_table\partition-table.bin
if errorlevel 1 goto :error
echo [3/3] App firmware...
python -m esptool --chip esp32 -p %PORT% -b 460800 --before default_reset --after hard_reset write_flash --flash_mode dio --flash_freq 40m --flash_size 2MB 0x10000 smarthome_esp32.bin
if errorlevel 1 goto :error
echo.
echo === Flash SUCCESS ===
exit /b 0
:error
echo.
echo === Flash FAILED ===
exit /b 1
