@echo off
set MSYSTEM=
if not defined IDF_PATH set "IDF_PATH=D:\Espressif\frameworks\esp-idf-v5.5.4"
if not defined IDF_PYTHON_ENV_PATH set "IDF_PYTHON_ENV_PATH=D:\Espressif\python_env\idf5.5_py3.11_env"
if not defined IDF_TOOLS_PATH set "IDF_TOOLS_PATH=D:\Espressif"
set "PATH=%IDF_PYTHON_ENV_PATH%\Scripts;%IDF_TOOLS_PATH%\tools\cmake\3.30.2\bin;%IDF_TOOLS_PATH%\tools\ninja\1.12.1;%IDF_TOOLS_PATH%\tools\xtensa-esp-elf\esp-14.2.0_20260121\xtensa-esp-elf\bin;%IDF_TOOLS_PATH%\tools\idf-git\2.44.0\cmd;%IDF_TOOLS_PATH%\tools\idf-python\3.11.2;%PATH%"
cd /d "%~dp0"
echo === Rebuilding SmartHome S7 ===
python "%IDF_PATH%\tools\idf.py" build
echo === Build exit: %ERRORLEVEL% ===
