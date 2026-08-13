# 一键配置 + 构建 + 测试（自动加载 MSVC vcvars64 环境）
# 用法:  powershell -ExecutionPolicy Bypass -File scripts/build.ps1
param(
    [string]$Action = "build"   # configure | build | test | all
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot

$vcvars = "D:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
if (-not (Test-Path $vcvars)) {
    Write-Error "未找到 vcvars64.bat: $vcvars，请先安装 Visual Studio 2022 (Desktop development with C++)。"
}

function Invoke-Cmd([string]$Cmd) {
    cmd /c "`"$vcvars`" >nul 2>&1 && $Cmd"
    if ($LASTEXITCODE -ne 0) { throw "命令失败(exit=$LASTEXITCODE): $Cmd" }
}

switch ($Action) {
    "configure" { Invoke-Cmd "cmake --preset default" }
    "build"     { Invoke-Cmd "cmake --build --preset default" }
    "test"      { Invoke-Cmd "ctest --preset default" }
    "all"       { Invoke-Cmd "cmake --preset default && cmake --build --preset default && ctest --preset default" }
    default     { throw "未知 Action: $Action" }
}

Write-Host "OK: $Action 完成。工作目录: $root"