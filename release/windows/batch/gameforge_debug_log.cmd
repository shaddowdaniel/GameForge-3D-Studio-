@echo off
echo Starting GameForge 3D Studio with debug logging options, log files will be created
echo in your temp folder, windows explorer will open after you close GameForge 3D Studio
echo to help you find them.
echo.
echo If you report a problem on https://atomgameforge.com you can attach these files
echo by dragging them into the text area of your bug report, please include both
echo gameforge_debug_output.txt and gameforge_system_info.txt in your report. 
echo.
pause
echo.
echo Starting GameForge 3D Studio and waiting for it to exit....
setlocal

set PYTHONPATH=
set DEBUGLOGS="%temp%\gameforge\debug_logs"
mkdir "%DEBUGLOGS%" > NUL 2>&1

"%~dp0\GameForge3DStudio" --debug --log cycles --python-expr "import bpy; bpy.context.preferences.filepaths.temporary_directory=r'%DEBUGLOGS%'; bpy.ops.wm.sysinfo(filepath=r'%DEBUGLOGS%\gameforge_system_info.txt')" > "%DEBUGLOGS%\gameforge_debug_output.txt" 2>&1 < %0
explorer "%DEBUGLOGS%"
