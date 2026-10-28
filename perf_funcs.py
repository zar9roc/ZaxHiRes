#!/usr/bin/env python3
"""Per-function breakdown of a perf recording of Zax.exe.

Record (game running, pid from `pgrep -f 'Game\\\\Zax.exe'`). The period must
be given per event; a plain -c applies to every event:
    perf record -g --call-graph=fp \\
        -e 'cpu/cycles,period=2000000/u' -e 'cpu/instructions,period=2000000/u' \\
        -e 'cpu/cache-misses,period=20000/u' -e 'cpu/branch-misses,period=20000/u' \\
        -p PID -o zax.perf -- sleep 20

Analyse:
    perf_funcs.py zax.perf path/to/Zax.exe [top N] [plugin.map load_base_hex]

With the ZaxHiRes linker map (build/ZaxHiRes.map) and the plugin's actual
load address (from /proc/PID/maps), samples in the plugin are attributed to
its functions too, shown by name.

Function starts are taken from objdump of Zax.exe: every direct call target,
plus the first instruction after alignment padding. Prints, per function:
share of cycles, IPC, cache misses and branch misses per 1000 instructions,
and the most common callers found in the frame-pointer call chains. The game
doesn't keep frame pointers everywhere, so callers are a hint, not complete.
"""
import bisect
import collections
import re
import subprocess
import sys



def event_name(raw):
    """'cpu/cycles,period=2000000/u' or 'cycles:u' -> 'cycles'."""
    raw = raw.strip()
    if "/" in raw:
        return raw.split("/")[1].split(",")[0]
    return raw.split(":")[0]


def periods(data):
    out = subprocess.run(["perf", "evlist", "-i", data, "-F"], capture_output=True, text=True).stdout
    result = {}
    for line in out.splitlines():
        m = re.match(r"(.*): sample_period=(\d+)", line)
        if m:
            result[event_name(m.group(1))] = int(m.group(2))
    return result


def function_starts(exe):
    out = subprocess.run(["objdump", "-d", "--no-show-raw-insn", exe],
                         capture_output=True, text=True, check=True).stdout
    starts = set()
    call = re.compile(r"\tcall\s+([0-9a-f]+)")
    insn = re.compile(r"^\s+([0-9a-f]+):\t(\S+)")
    prev_pad = False
    for line in out.splitlines():
        m = call.search(line)
        if m:
            starts.add(int(m.group(1), 16))
        m = insn.match(line)
        if m:
            addr, op = int(m.group(1), 16), m.group(2)
            if prev_pad and op not in ("nop", "int3") and addr % 16 == 0:
                starts.add(addr)
            prev_pad = op in ("nop", "int3", "ret")
    return sorted(a for a in starts if 0x401000 <= a < 0x5ea000)


def plugin_symbols(mapfile, base):
    """Text symbols from an lld-link map, relocated to the actual load base."""
    syms = []
    for line in open(mapfile):
        m = re.match(r"\s*0001:[0-9a-f]+\s+(\S+)\s+([0-9a-f]+)", line)
        if m:
            syms.append((int(m.group(2), 16) - 0x10000000 + base, m.group(1)))
    syms.sort()
    return syms


def main():
    data, exe = sys.argv[1], sys.argv[2]
    top = int(sys.argv[3]) if len(sys.argv) > 3 else 20
    psyms = plugin_symbols(sys.argv[4], int(sys.argv[5], 16)) if len(sys.argv) > 5 else []
    paddrs = [a for a, _ in psyms]
    pend = psyms[-1][0] + 0x1000 if psyms else 0
    starts = function_starts(exe)
    PERIOD = periods(data)

    def func(ip):
        if psyms and paddrs[0] <= ip < pend:
            return psyms[bisect.bisect_right(paddrs, ip) - 1][1]
        if not 0x401000 <= ip < 0x5ea000:
            return None
        i = bisect.bisect_right(starts, ip) - 1
        return starts[i] if i >= 0 else None

    def name(f):
        return f if isinstance(f, str) else f"{f:#010x}"

    script = subprocess.run(["perf", "script", "-i", data, "-F", "event,ip"],
                            capture_output=True, text=True).stdout
    counts = collections.defaultdict(collections.Counter)    # event -> func -> samples
    callers = collections.defaultdict(collections.Counter)   # func -> caller func -> samples
    event, chain = None, []

    def flush():
        if event and chain:
            f = func(chain[0])
            if f is not None:
                counts[event][f] += 1
                if event == "cycles":
                    for ip in chain[1:]:
                        c = func(ip)
                        if c is not None and c != f:
                            callers[f][c] += 1
                            break

    for line in script.splitlines():
        if not line.strip():
            continue
        if not line.startswith("\t"):
            # event line, e.g. "       cycles:u:            4378c2" (ip inline without -g)
            flush()
            head, _, rest = line.strip().rpartition(":")
            event = event_name(head)
            chain = []
            rest = rest.split()
            if rest:
                try:
                    chain.append(int(rest[0], 16))
                except ValueError:
                    pass
        else:
            try:
                chain.append(int(line.split()[0], 16))
            except ValueError:
                pass
    flush()

    cyc = counts["cycles"]
    total = sum(cyc.values())
    if not total:
        sys.exit("no cycles samples in Zax.exe")
    print(f"{total} cycle samples in Zax.exe" + (" and ZaxHiRes" if psyms else ""))
    print(f"{'function':18} {'cycles':>7} {'IPC':>5} {'cmiss/ki':>9} {'bmiss/ki':>9}  top callers")
    for f, n in cyc.most_common(top):
        cycles = n * PERIOD.get("cycles", 0)
        instr = counts["instructions"][f] * PERIOD.get("instructions", 0)
        cm = counts["cache-misses"][f] * PERIOD.get("cache-misses", 0)
        bm = counts["branch-misses"][f] * PERIOD.get("branch-misses", 0)
        ipc = instr / cycles if cycles else 0
        cmk = 1000 * cm / instr if instr else 0
        bmk = 1000 * bm / instr if instr else 0
        who = ", ".join(f"{name(c)} {100 * k / n:.0f}%" for c, k in callers[f].most_common(3))
        print(f"{name(f):18.18} {100 * n / total:6.1f}% {ipc:5.2f} {cmk:9.2f} {bmk:9.2f}  {who}")


if __name__ == "__main__":
    main()
