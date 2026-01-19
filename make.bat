@echo off
pushd "%~dp0"
chcp 65001 >nul
setlocal enabledelayedexpansion

:: --- 配置路径 ---
set "ROOT=%~dp0"
if "%ROOT:~-1%"=="\" set "ROOT=%ROOT:~0,-1%"
set "OUTDIR=%ROOT%/out"
set "GRUBDIR=%OUTDIR%/grub"
set "ISO=%ROOT%/xpage-x86-64.iso"
set "KERNEL=%ROOT%/Kernel/kernel.elf"

:: --- 检查依赖 ---
if not exist "%KERNEL%" (
    echo [ERROR] 未找到内核文件: %KERNEL%
    exit /b 1
)

:: --- 准备目录结构 ---
if not exist "%OUTDIR%" mkdir "%OUTDIR%"
if exist "%GRUBDIR%" rd /s /q "%GRUBDIR%"
mkdir "%GRUBDIR%"
mkdir "%GRUBDIR%\boot"
mkdir "%GRUBDIR%\boot\grub"

:: --- 复制内核文件 ---
copy /y "%KERNEL%" "%GRUBDIR%\boot\kernel.elf" >nul

:: --- 生成 grub.cfg ---
echo set timeout=0 > "%GRUBDIR%\boot\grub\grub.cfg"
echo set default=0 >> "%GRUBDIR%\boot\grub\grub.cfg"
echo menuentry "xpage OS" { >> "%GRUBDIR%\boot\grub\grub.cfg"
echo     multiboot2 /boot/kernel.elf >> "%GRUBDIR%\boot\grub\grub.cfg"
echo     boot >> "%GRUBDIR%\boot\grub\grub.cfg"
echo } >> "%GRUBDIR%\boot\grub\grub.cfg"

:: --- 生成 ISO 镜像（使用 WSL） ---
for /f "delims=" %%i in ('wsl whoami') do set WSLUSER=%%i
for /f "usebackq tokens=*" %%i in (`wsl wslpath -u "%ROOT%"`) do set "WSL_ROOT=%%i"
wsl -u %WSLUSER% -- bash -c "cd %WSL_ROOT% && grub-mkrescue -o xpage-x86-64.iso out"

if exist "%ISO%" (
    echo [DONE] ISO 镜像已生成: %ISO%
) else (
    echo [ERROR] ISO 镜像生成失败
    exit /b 1
)
