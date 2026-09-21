#!/usr/bin/env python3
"""Per-node, per-channel RSSI survey.

Switches all 8 nodes to one channel, then captures the raw RSSI at each
signal level you set on the quad. Results append to a CSV.

Run on the Pi:
    cd ~/RotorHazard/src/server
    ~/.venv/bin/python rssi_survey.py <command> [args]

Commands:
    tune R1              switch all nodes to band R channel 1
    measure R1 floor     capture current levels, label them "floor"
    measure R1 pit       ... "pit"
    measure R1 race      ... "race"
    show                 print everything collected so far
"""
import sys, time, csv, os, statistics as st
import socketio

URL = "http://localhost:5000"
CSV = os.path.expanduser("~/rh-data/rssi_survey.csv")
SETTLE = 3.0      # seconds to wait after tuning before trusting readings
SAMPLE = 8.0      # seconds of samples per measurement

BANDS = {}
for b in "RFEABDLU":
    for ch in range(1, 9):
        BANDS[f"{b}{ch}"] = (b, ch)

# RotorHazard IMD/raceband frequency table
FREQ = {
 "R1":5658,"R2":5695,"R3":5732,"R4":5769,"R5":5806,"R6":5843,"R7":5880,"R8":5917,
 "F1":5740,"F2":5760,"F3":5780,"F4":5800,"F5":5820,"F6":5840,"F7":5860,"F8":5880,
 "E1":5705,"E2":5685,"E3":5665,"E4":5645,"E5":5885,"E6":5905,"E7":5925,"E8":5945,
 "A1":5865,"A2":5845,"A3":5825,"A4":5805,"A5":5785,"A6":5765,"A7":5745,"A8":5725,
 "B1":5733,"B2":5752,"B3":5771,"B4":5790,"B5":5809,"B6":5828,"B7":5847,"B8":5866,
}

def connect():
    sio = socketio.Client()
    sio.connect(URL)
    return sio

def tune(chan):
    band, ch = BANDS[chan]
    freq = FREQ[chan]
    sio = connect()
    for n in range(8):
        sio.emit("set_frequency", {"node": n, "frequency": freq,
                                   "band": band, "channel": ch})
        time.sleep(0.25)
    time.sleep(1.0)
    sio.disconnect()
    print(f"all 8 nodes tuned to {chan} ({freq} MHz)")

def sample(seconds=SAMPLE):
    sio = connect()
    rows = []
    sio.on("heartbeat", lambda d: rows.append(d["current_rssi"])
           if d.get("current_rssi") else None)
    t = time.time()
    while time.time() - t < seconds:
        time.sleep(0.1)
    sio.disconnect()
    if not rows:
        raise SystemExit("no heartbeat data received")
    return rows

def measure(chan, level):
    print(f"settling {SETTLE}s ...")
    time.sleep(SETTLE)
    print(f"sampling {SAMPLE}s ...")
    rows = sample()
    new = not os.path.exists(CSV)
    with open(CSV, "a", newline="") as fh:
        w = csv.writer(fh)
        if new:
            w.writerow(["channel","level","node","mean","min","max","stdev","n"])
        print(f"\n{chan} / {level}   ({len(rows)} samples)")
        print("node    mean    min    max   stdev")
        for i in range(8):
            c = [r[i] for r in rows]
            m, lo, hi, sd = st.mean(c), min(c), max(c), st.pstdev(c)
            w.writerow([chan, level, i+1, f"{m:.1f}", lo, hi, f"{sd:.2f}", len(c)])
            print(f"  {i+1}  {m:7.1f} {lo:6d} {hi:6d}  {sd:6.2f}")
    print(f"\nappended to {CSV}")

def show():
    if not os.path.exists(CSV):
        raise SystemExit("no data yet")
    with open(CSV) as fh:
        data = list(csv.DictReader(fh))
    chans = sorted({d["channel"] for d in data})
    for chan in chans:
        print(f"\n=== {chan} ===")
        print("node   floor     pit    race   pit-floor  race-pit  race-floor")
        for n in range(1, 9):
            g = {d["level"]: float(d["mean"]) for d in data
                 if d["channel"] == chan and int(d["node"]) == n}
            f, p, r = g.get("floor"), g.get("pit"), g.get("race")
            def fmt(x): return f"{x:7.1f}" if x is not None else "      -"
            pf = f"{p-f:9.1f}" if p is not None and f is not None else "        -"
            rp = f"{r-p:9.1f}" if r is not None and p is not None else "        -"
            rf = f"{r-f:10.1f}" if r is not None and f is not None else "         -"
            print(f"  {n} {fmt(f)} {fmt(p)} {fmt(r)} {pf} {rp} {rf}")

if __name__ == "__main__":
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    cmd = sys.argv[1]
    if cmd == "tune":
        tune(sys.argv[2].upper())
    elif cmd == "measure":
        measure(sys.argv[2].upper(), sys.argv[3].lower())
    elif cmd == "at":
        # tune all nodes to one channel and capture one level there
        chan = sys.argv[2].upper()
        level = sys.argv[3].lower()
        tune(chan)
        measure(chan, level)
    elif cmd == "sweep":
        # capture one level across every channel of a band, unattended
        level = sys.argv[2].lower()
        band = sys.argv[3].upper() if len(sys.argv) > 3 else "R"
        chans = [f"{band}{i}" for i in range(1, 9)]
        print(f"sweeping {level} across {chans}")
        for chan in chans:
            tune(chan)
            measure(chan, level)
        print("\nsweep complete")
    elif cmd == "show":
        show()
    else:
        raise SystemExit(__doc__)
