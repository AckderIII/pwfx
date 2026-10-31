# The cases run.sh compares against the reference implementations.
for n in 1 2 3 4 5 8 16; do
    run_case lpf -r $RATE -c $CH -n $n -f 1000 -b 128
done
run_case lpf -r $RATE -c $CH -n 4 -f 50    -b 97
run_case lpf -r $RATE -c $CH -n 4 -f 20000 -b 1024

for n in 1 2 3 4 5 8 16; do
    run_case hpf -r $RATE -c $CH -n $n -f 80 -b 128
done
run_case hpf -r $RATE -c $CH -n 4 -f 20    -b 97
run_case hpf -r $RATE -c $CH -n 4 -f 20000 -b 1024

run_case eq -r $RATE -c $CH -b 128
run_case eq -r $RATE -c $CH -b 128 --ls 90:-4:0.7
run_case eq -r $RATE -c $CH -b 128 --hs 9000:2.5:0.7
run_case eq -r $RATE -c $CH -b 128 --b3 2500:3:0.9
run_case eq -r $RATE -c $CH -b 128 --b3 2500:3:0.9 --rbj
run_case eq -r $RATE -c $CH -b 97 --ls 90:-4:0.7 --b2 300:-3:1.0 --b3 2500:3:0.9 --b4 6000:-2:2 --hs 9000:2.5:0.7
run_case eq -r $RATE -c $CH -b 1024 --ls 30:12:1 --b2 120:-12:0.3 --b3 15000:6:4 --b4 19000:-9:8 --hs 18000:-6:1

run_case gate -r $RATE -c $CH -b 128
run_case gate -r $RATE -c $CH -b 128 -t -35 -o 150 -e 200
run_case gate -r $RATE -c $CH -b 97  -t -20 -T -30 -a 5 -o 50 -e 80 -k 10
run_case gate -r $RATE -c $CH -b 1024 -t -45 -g 30 -F 0 -G 8000
run_case gate -r $RATE -c $CH -b 256 -t -30 -k 0 -a 0 -e 0 -o 0
run_case gate -r $RATE -c $CH -b 333 -t -25 -k 40 -g 90 -F 200 -G 6000

run_case comp -r $RATE -c $CH -b 128
run_case comp -r $RATE -c $CH -b 128 -t -18 -R 3 -a 10 -e 120 -w 6 -m 6
run_case comp -r $RATE -c $CH -b 97  -t -24 -R 2 -a 25 -e 250 -w 10 -M
run_case comp -r $RATE -c $CH -b 256 -t -30 -R 20 -w 0 -k 0 -a 0 -e 0
run_case comp -r $RATE -c $CH -b 333 -t -12 -R 4 -q 30 -l avg -F 120
run_case comp -r $RATE -c $CH -b 512 -t -12 -R 4 -q 30 -l off -G 8000
run_case comp -r $RATE -c $CH -b 1024 -t -40 -R 8 -k 50 -m -6 -F 150 -G 6000

run_case limit -r $RATE -c $CH -b 128
run_case limit -r $RATE -c $CH -b 128 -C -1 -i 6 -e 60
run_case limit -r $RATE -c $CH -b 97  -C -0.3 -i 0 -e 100 -k 10
run_case limit -r $RATE -c $CH -b 256 -C -6 -i 18 -e 10 -k 1 -O 1
run_case limit -r $RATE -c $CH -b 333 -C -2 -i 12 -e 200 -k 0 -O 16
run_case limit -r $RATE -c $CH -b 1024 -C -20 -i 30 -e 5 -k 50 -O 2

# A plain delay, an echo, all wet and half wet, a zero time, and block sizes
# that do and do not divide the ring -- the delay is the one core whose state
# is addressed by position, so the block boundaries are where it would break.
run_case delay -r $RATE -c $CH -b 128
run_case delay -r $RATE -c $CH -b 128  -t 10
run_case delay -r $RATE -c $CH -b 97   -t 10
run_case delay -r $RATE -c $CH -b 1024 -t 0.5
run_case delay -r $RATE -c $CH -b 128  -t 120 -f 40 -m 35
run_case delay -r $RATE -c $CH -b 333  -t 7.3 -f 95 -m 50
run_case delay -r $RATE -c 1   -b 4096 -t 250 -f 60 -m 100
run_case delay -r $RATE -c $CH -b 1    -t 2.1 -f 20 -m 0

# The de-esser: both detections, both cut modes, listening, no lookahead and
# the longest, instant attack, the extremes of ratio, range and width, and
# block sizes from 1 frame to 4096. The test stream's noise and sweep run
# through the band, so these cut; its silence and its denormal tail are where
# the relative floor and the bit-exact pass-through are exercised.
run_case deess -r $RATE -c $CH -b 128
run_case deess -r $RATE -c $CH -b 97   -d abs -t -30
run_case deess -r $RATE -c $CH -b 1024 -m wide
run_case deess -r $RATE -c $CH -b 333  -L
run_case deess -r $RATE -c $CH -b 128  -k 0 -a 0 -e 5
run_case deess -r $RATE -c $CH -b 256  -f 3000 -Q 0.5 -R 20 -g 30
run_case deess -r $RATE -c 1   -b 4096 -f 9000 -Q 4 -t -6 -R 2 -g 6
run_case deess -r $RATE -c $CH -b 1    -k 10 -d abs -t -40 -m wide

# The noise suppressor: the reference reads a frame at a time, the core takes
# blocks of any size and frames them itself, so block sizes that do and do
# not divide the 480-sample frame are the point -- 1, 97, 480, 1024, 4096.
# Full reduction, none, a partial one, and the voice-activity gate.
run_case denoise -r $RATE -c $CH -b 128
run_case denoise -r $RATE -c $CH -b 97   -n 100
run_case denoise -r $RATE -c $CH -b 480  -n 0
run_case denoise -r $RATE -c 1   -b 1    -n 12
run_case denoise -r $RATE -c $CH -b 1024 --vad 0.6
run_case denoise -r $RATE -c $CH -b 4096 -n 60 --vad 0.9
