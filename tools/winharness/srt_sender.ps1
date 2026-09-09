$log = "$PSScriptRoot\srt_sender.log"
while ($true) { & 'C:\Users\uniongraphics\Documents\GitHub\QCView-Player\build-release\ffmpeg.exe' -hide_banner -loglevel error -re -f lavfi -i testsrc2=size=1280x720:rate=30 -f lavfi -i "sine=frequency=440" -c:v libx264 -preset veryfast -tune zerolatency -g 30 -pix_fmt yuv420p -c:a aac -f mpegts "srt://127.0.0.1:9000?mode=listener" 2>>$log; Start-Sleep 1 }
