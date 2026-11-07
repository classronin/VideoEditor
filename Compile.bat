@echo off
chcp 65001 >nul
REM ============================================================
REM  双击即编译 VideoEditor.exe
REM  它会先切到"本批处理所在的那个文件夹"(vse)，再跑 make。
REM  所以这个 .bat 放在 vse\ 里，放别处会找不到 Makefile。
REM ============================================================
cd /d "%~dp0"

REM 先确认两个工具在不在 PATH 里
where make >nul 2>nul
if errorlevel 1 (
    echo [缺 make] 请用装 MinGW 时自带的终端(如 mingw32-shell / msys)打开，
    echo          或把 MinGW 的 bin 目录加进系统 PATH，再双击本文件。
    pause
    exit /b 1
)
where g++ >nul 2>nul
if errorlevel 1 (
    echo [缺 g++] 同上：make 有了，但没找到编译器 g++。
    pause
    exit /b 1
)

echo 开始编译(4 路并行)...
make -j4
if errorlevel 1 (
    echo.
    echo 编译失败。请把上面带 error / warning 的红字整段贴出来。
    pause
    exit /b 1
)

echo.
echo 成功。产物: %~dp0bin\VideoEditor.exe  —— 双击即可运行。
pause
