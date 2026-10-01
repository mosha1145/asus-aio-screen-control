@echo off
chcp 65001 >nul
cd /d "D:\TUFScreenControl\PyTufGUI"

REM 选择带 PySide6 的 Python 解释器: 系统 Python 优先, 回退豆包运行时
set "PY="
for %%P in (
  "C:\Users\mosha\AppData\Local\Programs\Python\Python312\python.exe"
  "C:\Users\mosha\AppData\Local\Doubao\User Data\sandbox_runtime\bases\c98c5042338ed152c6f10ecd8591889f\python\python.exe"
) do (
  %%~P -c "import PySide6, cv2, numpy" >nul 2>&1
  if not errorlevel 1 set "PY=%%~P" & goto :run
)

echo [错误] 未找到带 PySide6 的 Python, 请先运行:
echo   pip install PySide6 opencv-python numpy
pause
exit /b 1

:run
echo 使用解释器: %PY%
start "" %PY% tuf_gui.py
exit /b 0
