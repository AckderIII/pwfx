#!/usr/bin/env python3
"""
protocol.py -- check the stdin/stdout control protocol itself, independently
of what any particular filter does to audio.

The contract being tested:
  * on start, one "name value" line per parameter and nothing else on stdout;
  * a line on stdin sets one parameter, in "name value", "name=value" or
    "name:value" form, and the new value is echoed back the same way;
  * a bare parameter name queries it; "dump" reprints everything; "help"
    prints usage; "quit" exits 0;
  * blank lines, #-comments and over-long lines disturb nothing;
  * everything on stdout is either a "name value" line or a #-comment, so a
    consumer can parse it -- the help text included;
  * bad input goes to stderr as "error: ..." and leaves the value alone.

The values to poke with are taken from each filter's own --help, so the test
stays inside every parameter's declared range as filters change.
"""

import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from live import Filter, check, fails                       # noqa: E402
from stress import discover                                 # noqa: E402


def drain(f):
    while f.errline(0.05) is not None:
        pass
    while f.line(0.05) is not None:
        pass


def main():
    bins = sys.argv[1:] or ["./bin/pw_comp"]

    for binpath in bins:
        name = os.path.basename(binpath)
        print("== %s: control protocol ==" % name)

        nums = [p for p in discover(binpath) if p[1] == "num"]
        if not nums:
            check(False, "%s declares a numeric parameter" % name)
            continue
        k, _, (lo, hi) = nums[0]

        f = Filter([binpath, "-c", "1", "--node-name", name + "-proto"])

        dump = []
        while True:
            l = f.line(10.0)
            if l is None or l.startswith("#"):
                break
            dump.append(l)
        check(len(dump) > 0 and all(len(l.split(" ", 1)) == 2 for l in dump),
              "startup dump is %d 'name value' lines" % len(dump))

        # A declared range is not the same as an acceptable value: a cutoff
        # of 115 kHz is inside 1..384000 and still above Nyquist at 48 kHz.
        # This tests the protocol, not the value policy, so take the first
        # two settings the filter actually accepts.
        accepted = []
        for frac in (0.30, 0.60, 0.10, 0.05, 0.02, 0.01, 0.9, 0.75):
            v = round(lo + frac * (hi - lo), 3)
            if v in accepted:
                continue
            got = f.set("%s %g" % (k, v))
            if got is not None and got.split(" ")[0] == k:
                accepted.append(v)
            drain(f)
            if len(accepted) == 2:
                break
        if not check(len(accepted) == 2,
                     "found two settable values for '%s'" % k, str(accepted)):
            f.stop()
            print()
            continue
        v1, v2 = accepted

        # the three accepted separators, plus surrounding whitespace, all of
        # which must land on the same value
        echo = None
        for form in ("%s=%g", "%s:%g", "   %s    %g   "):
            cmd = form % (k, v1)
            got = f.set(cmd)
            ok = got is not None and got.split(" ")[0] == k
            if ok and echo is None:
                echo = got
            check(ok and got == echo, "%-26s -> %s" % (cmd.strip(), echo),
                  "got %r" % got)

        got = f.set(k)
        check(got == echo, "a bare name queries the value", "got %r" % got)

        # things that must change nothing and say nothing on stdout
        for quiet in ("", "   ", "# a comment", "#", "x" * 5000 + " 1"):
            f.send(quiet)
        time.sleep(0.3)
        stray = f.line(0.3, skip_comments=True)
        check(stray is None, "blank, comment and over-long lines are ignored",
              "got %r" % stray)
        drain(f)

        got = f.set("%s %g" % (k, v2))
        check(got is not None and got.split(" ")[0] == k and got != echo,
              "still accepts settings afterwards", "got %r" % got)
        good = got

        f.send("dump")
        again = []
        while True:
            l = f.line(1.0, skip_comments=True)
            if l is None:
                break
            again.append(l)
        check(len(again) == len(dump),
              "dump reprints all %d parameters" % len(dump),
              "got %d" % len(again))

        # every line of help is a comment, or a consumer cannot parse stdout
        f.send("help")
        nhelp = nbad = 0
        while True:
            l = f.line(1.0)
            if l is None:
                break
            nhelp += 1
            if not l.startswith("#"):
                nbad += 1
        check(nhelp > 10 and nbad == 0,
              "help is %d lines, all #-comments" % nhelp,
              "%d lines are not comments" % nbad)

        drain(f)
        for bad, why in (("nosuchparam 1", "unknown parameter"),
                         ("%s banana" % k, "unparseable value"),
                         ("%s %g" % (k, hi + abs(hi) + 1000), "out of range"),
                         ("rate 44100", "read-only"),
                         ("channels 3", "command line only")):
            f.send(bad)
            e = f.errline(2.0)
            check(e is not None and e.startswith("error:"),
                  "rejects %-24s (%s)" % (bad, why), str(e))

        got = f.set(k)
        check(got == good, "a rejected setting leaves the value alone",
              "got %r, wanted %r" % (got, good))

        rc = f.stop()
        check(rc == 0, "quit exits 0", "rc=%s" % rc)
        print()

    print("control protocol: %d failure(s)" % len(fails))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
