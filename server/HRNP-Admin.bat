@echo off
rem HarmonyRecomp online server - admin menu (needs Python and the Windows ssh client)
cd /d "%~dp0"
where py >nul 2>nul && (py -3 hrnp_admin.py %* & goto end)
python hrnp_admin.py %*
:end
pause
