#!/bin/bash
# Diff each filter's DSP core against the reference implementation it came
# from, byte for byte, over the same test stream.
set -u
cd "$(dirname "$0")/.."
S=${TMPDIR:-/tmp}/pwf-dsp.$$
mkdir -p "$S"
trap 'rm -rf "$S"' EXIT

RATE=48000
CH=2
python3 tests/gen.py $RATE $CH 4 > "$S/in.raw" || exit 1
echo "test signal: $(stat -c%s "$S/in.raw") bytes, ${RATE} Hz, ${CH} ch"

fails=0
run_case() {           # name  args...
    local name=$1; shift
    ./bin/ref/"$name"  "$@" < "$S/in.raw" > "$S/ref.raw" 2>/dev/null
    ./bin/t_"$name"    "$@" < "$S/in.raw" > "$S/new.raw" 2>/dev/null
    if cmp -s "$S/ref.raw" "$S/new.raw"; then
        printf '  ok   %-6s %s\n' "$name" "$*"
    else
        printf '  FAIL %-6s %s  (%s vs %s bytes)\n' "$name" "$*" \
               "$(stat -c%s "$S/ref.raw")" "$(stat -c%s "$S/new.raw")"
        cmp "$S/ref.raw" "$S/new.raw" 2>&1 | head -1 | sed 's/^/        /'
        fails=$((fails+1))
    fi
}

. tests/cases.sh

# The limiter's whole point is that the ceiling is a guarantee, not a
# tendency. Its final clamp is a safety net that should never engage, so run
# it hard into the ceiling and insist the breach count is zero.
echo
echo "limiter ceiling guarantee:"
for args in "-C -1 -i 24 -e 30 -k 5" "-C -0.1 -i 40 -e 5 -k 1" \
            "-C -12 -i 30 -e 200 -k 20 -O 16" "-C -3 -i 18 -e 60 -k 0 -O 1"; do
    msg=$(./bin/t_limit -r $RATE -c $CH -b 128 -v $args \
          < "$S/in.raw" > /dev/null 2>&1 \
          || true; ./bin/t_limit -r $RATE -c $CH -b 128 -v $args \
          < "$S/in.raw" 2>&1 >/dev/null)
    breaches=$(printf '%s' "$msg" | sed -n 's/.*breaches \([0-9]*\).*/\1/p')
    if [ "$breaches" = "0" ]; then
        printf '  ok   %s  -- %s\n' "$args" "$msg"
    else
        printf '  FAIL %s  -- %s\n' "$args" "$msg"
        fails=$((fails+1))
    fi
done

# The sidechain compressor has no reference of its own: it is the
# compressor's core with a key, held to reference/comp.c on streams where the
# original hears exactly that key, and then to the design where they differ.
echo
echo "sidechain compressor against reference/comp.c and its design:"
python3 tests/sccomp.py
fails=$((fails+$?))

# The comparison above proves the delay core agrees with its reference;
# this proves what they agree on is right.
echo
echo "delay impulse response against theory:"
python3 tests/impulse.py
fails=$((fails+$?))

# Likewise the de-esser: the comparison proves core and reference agree, this
# that they cut what the design equations say.
echo
echo "de-esser against its design equations:"
python3 tests/deess.py
fails=$((fails+$?))

# The leveler: the comparison proves core and reference agree, this that
# they read loudness as BS.1770 does and ride it as the design says -- on
# tones, and on real speech played 20 dB apart.
echo
echo "leveler against BS.1770 and its design:"
python3 tests/level.py
fails=$((fails+$?))

# The noise suppressor: the comparison proves the framing, this measures what
# it is for -- on real speech, and that RNNoise's delay is what it assumes.
echo
echo "noise suppressor on speech and noise:"
python3 tests/denoise.py
fails=$((fails+$?))

# The response curves are drawn from the same coefficients the DSP runs, so
# they are checked here too -- against theory, not against a reference.
echo
echo "response curves against theory:"
python3 tests/curves.py
fails=$((fails+$?))

echo
if [ $fails -eq 0 ]; then echo "all cases pass"
else echo "$fails case(s) failed"; fi
exit $fails
