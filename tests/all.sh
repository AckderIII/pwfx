#!/bin/bash
# Everything, in order of how much it needs to be true:
#
#   1. the DSP cores are byte-identical to the reference implementations
#      they were ported from, and the limiter's ceiling never breaks;
#   2. the control protocol on stdin/stdout behaves as documented;
#   3. each filter, running in the real graph, has the response it claims;
#   4. each filter survives its parameters being hammered under load.
#
# Steps 3 and 4 need a running PipeWire daemon and pw-cat/pw-link; they take
# a few minutes because each one plays and records real audio.
set -u
cd "$(dirname "$0")/.."

fails=0
step() { echo; echo "=================== $* ==================="; }
run()  { "$@" || fails=$((fails+1)); }

step "build"
run make -s all refs $(for f in tests/t_*.c; do
        echo "bin/$(basename "$f" .c)"; done)

step "1. DSP cores against the reference implementations"
run tests/run.sh

if [ "${PWF_OFFLINE_ONLY:-0}" = 1 ]; then
    echo; echo "PWF_OFFLINE_ONLY=1: skipping the live PipeWire tests"
    exit $fails
fi

step "2. control protocol"
run python3 tests/protocol.py bin/pw_lpf bin/pw_hpf bin/pw_eq \
                              bin/pw_gate bin/pw_comp bin/pw_limit bin/pw_gain \
                              bin/pw_delay bin/pw_deess bin/pw_denoise \
                              bin/pw_sccomp

step "3. live response in the graph"
# 200 Hz and 8 kHz against a 4th-order Butterworth at 1 kHz: three octaves up
# is 24 dB/oct x 3 = 72 dB, plus the extra the bilinear transform adds near
# the top of the band.
run python3 tests/live.py bin/pw_lpf -c 2 -f 1000 -n 4 --meter \
    -- "cutoff 1000" "cutoff 1000" "order 4" "order 4" \
       "cutoff 99999999" ERROR "order abc" ERROR "rate 44100" ERROR \
       "in-level 0" ERROR \
    --audio "200,8000" "200=0+-0.6,8000=-72+-7"

run python3 tests/live.py bin/pw_hpf -c 2 -f 1000 -n 4 \
    -- "bypass on" "bypass on" "bypass off" "bypass off" \
    --audio "125,4000" "125=-72+-7,4000=0+-0.6"

run python3 tests/live.py bin/pw_eq -c 2 --b3-freq 1000 --b3-q 2 --b3-gain 6 \
    -- "bells rbj" "bells rbj" "bells orfanidis" "bells orfanidis" \
       "bells nonsense" ERROR "hs-gain 20" "hs-gain 20" "hs-slope 6" ERROR \
    --audio "200,1000" "200=0+-0.8,1000=6+-0.4"

# wide open: the gate passes the signal untouched, and reports its lookahead
run python3 tests/live.py bin/pw_gate -c 2 -t -60 -k 5 \
    -- "lookahead 10" "lookahead 10" "latency" "latency 480" \
       "lookahead 5" "lookahead 5" "sc-lpf 50" ERROR \
    --audio "200,4000" "200=0+-0.5,4000=0+-0.5"
# shut: range 120 dB snaps the closed state to exact silence
run python3 tests/live.py bin/pw_gate -c 2 -t 0 -g 120 -F 0 \
    -- --audio "200,4000" "200=-240+-1,4000=-240+-1"
# shut at a 20 dB range: exactly 20 dB down, not muted -- and gr reads 20
run python3 tests/live.py bin/pw_gate -c 2 -t 0 -g 20 -F 0 \
    -- --audio "200,4000" "200=-20+-1.5,4000=-20+-1.5" --gr "20+-0.2"

# a 1 kHz tone at -10.46 dBFS, threshold -30, ratio 4, hard knee:
# (30 - 10.46) x (1 - 1/4) = 14.66 dB of gain reduction, and the gr meter
# has to say so
run python3 tests/live.py bin/pw_comp -c 2 -t -30 -R 4 -w 0 -a 1 -e 200 -k 5 \
    -- "link avg" "link avg" "link off" "link off" "link max" "link max" \
       "link sideways" ERROR "ratio 0.5" ERROR "gr 3" ERROR \
    --audio "1000" "1000=-14.66+-0.7" --gr "14.66+-0.5"
# the same, with 6 dB of makeup on top -- which the gr meter does not count
run python3 tests/live.py bin/pw_comp -c 2 -t -30 -R 4 -w 0 -a 1 -e 200 -m 6 \
    -- --audio "1000" "1000=-8.66+-0.7" --gr "14.66+-0.5"

# The sidechain compressor with nothing on sc_in: the same tone 20 dB over
# threshold, and no reduction at all -- the detector hears the key, never
# the audio, and an unlinked key is silence. 6 dB of makeup is all it adds.
run python3 tests/live.py bin/pw_sccomp -c 2 -t -30 -R 4 -w 0 -m 6 \
    -- "link max" ERROR "ratio 0.5" ERROR "gr 3" ERROR \
    --audio "1000" "1000=6+-0.3" --gr "0+-0.05"
# and with a key on sc_in: the audio cut by what the key's level demands,
# the key itself nowhere in the output
run python3 tests/sidechain.py bin/pw_sccomp

# below the ceiling: untouched, and latency is lookahead + 12 taps of
# interpolator group delay
run python3 tests/live.py bin/pw_limit -c 2 -C -1 -i 0 \
    -- "latency" "latency 252" --audio "1000" "1000=0+-0.3"
# driven 20 dB in: the ceiling holds exactly, at both settings
# --meter also pins out-level against the ceiling: the limiter's own meter
# has to agree with what was recorded off its output ports
# 20 dB in on a -10.46 dBFS tone is +9.54; a -1 ceiling takes 10.54 off it
run python3 tests/live.py bin/pw_limit -c 2 -C -1 -i 20 --meter \
    -- "oversample 3" ERROR "ceiling 3" ERROR \
    --audio "1000" "1000=9.46+-0.35" --peak "-1+-0.25" --gr "10.54+-0.35"
run python3 tests/live.py bin/pw_limit -c 2 -C -6 -i 20 -e 30 -k 10 \
    -- --audio "1000" "1000=4.46+-0.35" --peak "-6+-0.25"

# pw_gain has no reference implementation, so this is its ground truth: a
# plain -12 dB through live.py, and then each channel's level and polarity
# and the absence of clicks, measured in gain.py
run python3 tests/live.py bin/pw_gain -c 2 -g -12 \
    -- "gain 30" ERROR "invert sideways" ERROR "mute on" "mute on" "mute off" "mute off" \
    --audio "1000" "1000=-12+-0.3"
run python3 tests/gain.py bin/pw_gain

# The de-esser in the graph, from its design equations: a 0.3 tone at the
# band's centre is -13.47 dB RMS; absolute detection at -30 dB, ratio 4,
# cuts (30 - 13.47) x 3/4 = 12.40 dB there, and gr has to agree. Relative, a
# 200 Hz tone beside it puts the band 2.80 dB under the whole at the peak of
# a 5 ms RMS (see tests/deess.py), so threshold -12 at ratio 2 cuts 4.60 off
# the 6.5 kHz tone and leaves the 200 Hz one alone. That case does not
# check gr: the stream switching on and off as it is linked is a broadband
# click, which relative detection rightly cuts harder for a moment, and gr
# reports its highest reading. The audio pins the steady cut -- at the
# centre the cut is g itself -- and the first case pins gr against it.
run python3 tests/live.py bin/pw_deess -c 2 -d abs -t -30 -R 4 -g 30 \
    -- "freq 30000" ERROR "detect loud" ERROR "gr 3" ERROR \
    --audio "6500" "6500=-12.40+-0.3" --gr "12.40+-0.3"
run python3 tests/live.py bin/pw_deess -c 2 -t -12 -R 2 -g 30 \
    -- --audio "200,6500" "200=0+-0.3,6500=-4.60+-0.3"

# The noise suppressor in the graph: at reduction 0 a tone comes through
# untouched, which proves the framing live and the 48 kHz it asked for; its
# latency is the three frames it reports. What it does to speech and noise
# is measured offline on real speech, in tests/denoise.py.
run python3 tests/live.py bin/pw_denoise -c 2 -n 0 \
    -- "latency" "latency 1440" "reduction 150" ERROR "gr 3" ERROR \
    --audio "1000" "1000=0+-0.3"

# the delay measured against itself, so the graph's own timing drops out:
# exact to the sample, reported as no latency, and no click on a time change
run python3 tests/delay.py bin/pw_delay

# Bypassing a lookahead filter keeps its timing and its crossfade clean:
# impulse gaps unchanged through bypass on and off, and the tone at half a
# period of the latency -- the one an undelayed dry signal cancels -- level.
run python3 tests/bypass.py bin/pw_comp bin/pw_sccomp bin/pw_gate bin/pw_limit bin/pw_deess \
    bin/pw_denoise

# out-level with nothing connected to the output: the level the filter
# produces, from its loudest channel
run python3 tests/meter.py bin/pw_gain

step "4. filters composed into a chain"
run python3 tests/chain.py

step "5. parameters under load"
for b in pw_lpf pw_hpf pw_eq pw_gate pw_limit pw_gain pw_delay pw_deess pw_denoise; do
    run python3 tests/stress.py bin/$b -c 2
done
# The compressor is a gain stage: its output is the input times
# makeup_lin x 10^(-gr/20) with gr >= 0, so it can never exceed the input
# times its makeup. makeup reaches +40 dB, and auto-makeup adds -curve(0),
# largest at threshold -80 and ratio 100: 80 - 80/100 = 79.2 dB. Random
# settings land there often enough that a fixed bound made this flaky; the
# bound that means something is the one the settings themselves imply.
run python3 tests/stress.py --max-gain 119.2 bin/pw_comp -c 2
run python3 tests/stress.py --max-gain 119.2 bin/pw_sccomp -c 2

echo
echo "==========================================================="
if [ $fails -eq 0 ]; then echo "everything passed"
else echo "$fails step(s) failed"; fi
exit $fails
