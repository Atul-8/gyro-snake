@echo off
cd /d "%~dp0"
python imu_monitor.py --port COM65
if errorlevel 1 pause