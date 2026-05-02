#!/system/bin/sh
# QCON: persistent UDP sink on Pixel rmnet1.
# Runs forever — single shot nc + while loop. Called by bench script:
#   adb push qcon_pixel_sink.sh /data/local/tmp/
#   adb shell "nohup /system/bin/sh /data/local/tmp/qcon_pixel_sink.sh PORT >/dev/null 2>&1 &"
PORT=${1:-5201}
while true; do
    nc -u -l -p "$PORT" >/dev/null 2>&1
done
