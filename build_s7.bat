@echo off
set MSYSTEM=
if not defined IDF_PATH set "IDF_PATH=D:\Espressif\frameworks\esp-idf-v5.5.4"
if not defined IDF_PYTHON_ENV_PATH set "IDF_PYTHON_ENV_PATH=D:\Espressif\python_env\idf5.5_py3.11_env"
if not defined IDF_TOOLS_PATH set "IDF_TOOLS_PATH=D:\Espressif"
set "PATH=%IDF_PYTHON_ENV_PATH%\Scripts;%IDF_TOOLS_PATH%\tools\idf-git\2.44.0\cmd;%IDF_TOOLS_PATH%\tools\idf-python\3.11.2;%PATH%"
cd /d "%~dp0"
echo === Building SmartHome S7 ===
echo IDF_PATH=%IDF_PATH%
echo Python:
python --version
python "%IDF_PATH%\tools\idf.py" build
echo === Build exit code: %ERRORLEVEL% ===
