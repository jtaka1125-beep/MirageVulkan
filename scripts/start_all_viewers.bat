@echo off
echo Starting viewers for 3 devices...

start "Viewer 60000" python hybrid_video_viewer.py --port 60000
start "Viewer 60001" python hybrid_video_viewer.py --port 60001
start "Viewer 60002" python hybrid_video_viewer.py --port 60002

echo All viewers started!
pause
