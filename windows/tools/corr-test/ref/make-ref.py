"""Reference data for tools/corr-test (speaker correction): written by Avium Measure's own code.

Run with Avium Measure's Python (it imports avium_measure read-only and writes only into this folder):

    "<Avium Measure>\\.venv\\Scripts\\python.exe" -I -B make-ref.py "<Avium Measure folder>"

Writes, next to this script:
  studio.json      a two-channel profile like the Correction mode exports (cuts, shelves, a disabled
                   filter, a trim and a delay), every rate
  max20.json       twenty filters on each of L and R (the benchmark's load, and the most a profile may hold)
  alltypes.json    one channel with every filter type
  cases.json       profiles validate() must refuse or accept, with exactly the problems it reports
  engine-ref.bin   input and output of avium_measure.dsp.biquad.apply (scipy sosfilt, float64) for each
                   channel of studio.json and max20.json at 48 and 192 kHz, with the trim, preamp and delay:
                   "CRF1", frames n, records; x[n]; per record: rate, linear gain, delay frames, sections k,
                   k x (b0 b1 b2 a1 a2), y[n] (all little-endian, doubles float64)
  peaks.txt        peak_gain_db of each channel (5 Hz to Nyquist at 48 kHz, as validate() measures it; and the
                   same at 192 kHz, "<name>192", where the driver's measure of LF poles is hardest)
"""
import copy
import json
import math
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, sys.argv[1] if len(sys.argv) > 1 else r"C:\Users\aaron\Desktop\Avium\Avium Measure")

import numpy as np  # noqa: E402

from avium_measure.core.profile import (CorrectionProfile, ChannelCorrection, validate)  # noqa: E402
from avium_measure.dsp import biquad as bq  # noqa: E402


def studio():
    L = ChannelCorrection("L", "Left", 0, 0.0, 0.0, [
        bq.Biquad("peaking", 45.3, -8.2, 5.1), bq.Biquad("peaking", 87.5, -5.4, 6.8),
        bq.Biquad("peaking", 131.0, -3.1, 3.3), bq.Biquad("peaking", 240.0, -2.2, 1.9),
        bq.Biquad("high_shelf", 5000.0, 2.5, bq.BUTTERWORTH_Q), bq.Biquad("highpass_1", 18.0),
        bq.Biquad("peaking", 2400.0, 1.2, 1.6, enabled=False)])
    R = ChannelCorrection("R", "Right", 1, -0.8, 0.42, [
        bq.Biquad("peaking", 38.6, -9.5, 8.2), bq.Biquad("peaking", 71.0, -6.0, 4.0),
        bq.Biquad("peaking", 156.0, -2.7, 2.4), bq.Biquad("low_shelf", 300.0, -1.5, 0.9),
        bq.Biquad("high_shelf", 6200.0, 1.8, bq.BUTTERWORTH_Q), bq.Biquad("lowpass", 21000.0, 0.0, bq.BUTTERWORTH_Q)])
    p = CorrectionProfile("Studio A, listening position", [L, R], id="5f0c2d0e9a4b4b7c8e1f3a2b6c7d8e9f",
                          created="2026-11-15T02:10:00Z", device="ASIO::Apollo (Avium)",
                          measurement={"mode": "standard", "sample_rate_hz": 48000.0,
                                       "target": {"name": "Studio tilt", "tilt_db_per_oct": -0.8, "pivot_hz": 1000.0,
                                                  "points": []},
                                       "limits": {"f_lo": 20.0, "f_hi": 16000.0, "max_boost_db": 6.0},
                                       "timing": "loopback", "spots": [{"x": 0, "y": 0}] * 9})
    p.auto_preamp()
    return p


def max20():
    def chan(cid, name, out, trim, delay, seed):
        rng = np.random.default_rng(seed)
        fl = []
        for k in range(20):
            f = float(round(25.0 * 2 ** (k * 0.5) * (1 + 0.05 * rng.standard_normal()), 2))
            if k % 5 == 4:
                fl.append(bq.Biquad("low_shelf" if k < 10 else "high_shelf", f, float(round(-2.0 + rng.random(), 2)), 0.7071))
            else:
                fl.append(bq.Biquad("peaking", f, float(round(-1.0 - 7.0 * rng.random(), 2)),
                                    float(round(1.0 + 9.0 * rng.random(), 3))))
        return ChannelCorrection(cid, name, out, trim, delay, fl)
    p = CorrectionProfile("Twenty filters each", [chan("L", "Left", 0, 0.0, 0.0, 1), chan("R", "Right", 1, -0.4, 1.25, 2)],
                          id="0123456789abcdef0123456789abcdef", created="2026-11-15T03:00:00Z")
    p.auto_preamp()
    return p


def alltypes():
    C = ChannelCorrection("L", "Left", 0, 0.0, 0.0, [
        bq.Biquad("peaking", 63.0, -9.0, 6.0), bq.Biquad("low_shelf", 120.0, 5.0, 0.7071),
        bq.Biquad("high_shelf", 9000.0, 4.0, 0.5), bq.Biquad("lowpass", 18000.0, 0.0, 0.7071),
        bq.Biquad("highpass", 30.0, 0.0, 0.7071), bq.Biquad("lowpass_1", 19000.0), bq.Biquad("highpass_1", 15.0),
        bq.Biquad("low_shelf_1", 200.0, 3.0), bq.Biquad("high_shelf_1", 3000.0, -6.0), bq.Biquad("notch", 1500.0, 0.0, 4.0),
        bq.Biquad("allpass", 500.0, 0.0, 0.9), bq.Biquad("bandpass", 250.0, 0.0, 0.2),
        bq.Biquad("peaking", 20.0, -10.0, 10.0)])
    p = CorrectionProfile("Every type", [C], id="ffffffffffffffffffffffffffffffff", created="2026-11-15T04:00:00Z")
    p.auto_preamp()
    return p


def cases(good):
    out = []

    def add(name, fn=None, raw=None):
        d = copy.deepcopy(good)
        if fn is not None:
            fn(d)
        text = raw if raw is not None else json.dumps(d, allow_nan=False)
        out.append({"name": name, "text": text, "problems": validate(json.loads(text))})

    def f0(d, ch=0, i=0):
        return d["channels"][ch]["filters"][i]

    add("good")
    add("format", lambda d: d.update(format="something-else"))
    add("format missing", lambda d: d.pop("format"))
    add("version newer", lambda d: d.update(version=2))
    add("version zero", lambda d: d.update(version=0))
    add("version float", raw=json.dumps(good).replace('"version": 1,', '"version": 1.0,'))
    add("version bool", lambda d: d.update(version=True))
    add("not an object", raw="[1, 2, 3]")
    add("preamp positive", lambda d: d.update(preamp_db=3.0))
    add("preamp too low", lambda d: d.update(preamp_db=-61.0))
    add("preamp string", lambda d: d.update(preamp_db="-3"))
    add("no channels", lambda d: d.update(channels=[]))
    add("channels not a list", lambda d: d.update(channels={"L": 1}))
    add("channel not an object", lambda d: d["channels"].append(5))
    add("channel without id", lambda d: d["channels"][1].pop("id"))
    add("duplicate id", lambda d: d["channels"][1].update(id="L"))
    add("output negative", lambda d: d["channels"][0].update(output=-1))
    add("output float", lambda d: d["channels"][0].update(output=1.5))
    add("trim positive", lambda d: d["channels"][1].update(gain_db=1.0))
    add("delay negative", lambda d: d["channels"][1].update(delay_ms=-0.5))
    add("delay too long", lambda d: d["channels"][1].update(delay_ms=150.0))
    add("too many filters", lambda d: d["channels"][0].update(filters=d["channels"][0]["filters"] * 3))
    add("filters not a list", lambda d: d["channels"][0].update(filters={"a": 1}))
    add("unknown type", lambda d: f0(d).update(type="magic"))
    add("filter not an object", lambda d: d["channels"][0]["filters"].insert(0, 7))
    add("freq negative", lambda d: f0(d).update(freq_hz=-1))
    add("freq at nyquist", lambda d: f0(d).update(freq_hz=24000))
    add("gain too big", lambda d: f0(d).update(gain_db=31.0))
    add("gain missing", lambda d: f0(d).pop("gain_db"))
    add("q too small", lambda d: f0(d).update(q=0.01))
    add("q too big", lambda d: f0(d).update(q=51))
    add("48000 missing", lambda d: f0(d)["coefficients"].pop("48000"))
    add("coefficients not an object", lambda d: f0(d).update(coefficients=[1, 2, 3]))
    add("four coefficients", lambda d: f0(d)["coefficients"].__setitem__("96000", [1, 0, 0, 0]))
    add("infinite coefficient", raw=json.dumps(good).replace(
        json.dumps(good["channels"][0]["filters"][0]["coefficients"]["48000"][3]), "1e999", 1))
    add("string coefficient", lambda d: f0(d)["coefficients"]["48000"].__setitem__(0, "1"))
    add("unstable", lambda d: f0(d)["coefficients"].__setitem__("48000", [1, 0, 0, -2.5, 1.6]))
    add("unstable on the circle", lambda d: f0(d)["coefficients"].__setitem__("192000", [1, 0, 0, -1.0, 0.0]))
    add("params changed", lambda d: f0(d).update(gain_db=-3.0))
    add("one rate off", lambda d: f0(d)["coefficients"]["96000"].__setitem__(0, f0(d)["coefficients"]["96000"][0] + 1e-3))
    add("unknown rate", lambda d: f0(d)["coefficients"].__setitem__("12345", f0(d)["coefficients"]["48000"]))
    add("clip", lambda d: d.update(preamp_db=0.0))
    add("clip one channel", lambda d: d["channels"][0]["filters"].append(
        {"type": "peaking", "enabled": True, "freq_hz": 1000.0, "gain_db": 6.0, "q": 1.0,
         "coefficients": {"48000": list(bq.Biquad("peaking", 1000.0, 6.0, 1.0).coefficients(48000))}}))
    add("disabled boost is fine", lambda d: d["channels"][0]["filters"].append(
        {"type": "peaking", "enabled": False, "freq_hz": 1000.0, "gain_db": 6.0, "q": 1.0,
         "coefficients": {"48000": list(bq.Biquad("peaking", 1000.0, 6.0, 1.0).coefficients(48000))}}))
    add("extra fields ignored", lambda d: d.update(future={"x": 1}, notes="hello"))
    add("several problems", lambda d: (d.update(preamp_db=1.0), d["channels"][1].update(gain_db=2.0, delay_ms=-1)))
    return out


def engine_ref(profiles):
    """For each channel: the driver's signal path out = filters(delay(in * gain)) at the given rate,
    with avium_measure.dsp.biquad.apply (scipy sosfilt in float64)."""
    n = 8192
    rng = np.random.default_rng(1234)
    x = np.zeros(n)
    x[0] = 1.0e8
    x[1024:] = rng.standard_normal(n - 1024) * 1.5e8
    x[4096:4096 + 2048] += 6.0e8 * np.sin(2 * np.pi * 63.0 / 48000.0 * np.arange(2048))
    x = np.clip(np.round(x), -2147483648.0, 2147483647.0)     # the ring holds 32-bit integers
    recs = []
    for p in profiles:
        for fs in (48000, 192000):
            for c in p.channels:
                g_db = p.preamp_db + c.gain_db
                d = int(round(c.delay_ms * fs / 1000.0))
                xd = np.concatenate([np.zeros(d), x])[:n]
                y = bq.apply(c.filters, xd, fs, g_db)
                sos = [b.coefficients(fs) for b in c.filters if b.enabled]
                recs.append((fs, 10.0 ** (g_db / 20.0), d, sos, y))
    with open(os.path.join(HERE, "engine-ref.bin"), "wb") as f:
        f.write(struct.pack("<4sII", b"CRF1", n, len(recs)))
        f.write(struct.pack(f"<{n}d", *x))
        for fs, g, d, sos, y in recs:
            f.write(struct.pack("<IdII", fs, g, d, len(sos)))
            for c in sos:
                f.write(struct.pack("<5d", *[float(v) for v in c]))
            f.write(struct.pack(f"<{n}d", *y))


def main():
    st, mx, at = studio(), max20(), alltypes()
    for name, p in (("studio.json", st), ("max20.json", mx), ("alltypes.json", at)):
        d = p.to_dict()
        d["generator"] = "Avium Measure 1.0.0"
        assert validate(d) == [], (name, validate(d))
        with open(os.path.join(HERE, name), "w", encoding="utf-8", newline="\n") as f:
            json.dump(d, f, indent=2)
    good = json.load(open(os.path.join(HERE, "studio.json"), encoding="utf-8"))
    with open(os.path.join(HERE, "cases.json"), "w", encoding="utf-8", newline="\n") as f:
        json.dump(cases(good), f, indent=1)
    engine_ref([st, mx])
    with open(os.path.join(HERE, "peaks.txt"), "w", encoding="utf-8", newline="\n") as f:
        for name, p in (("studio", st), ("max20", mx), ("alltypes", at)):
            f.write(f"{name} preamp {p.preamp_db!r}\n")
            for c in p.channels:
                f.write(f"{name} {c.id} {c.peak_db(48000)!r} {c.gain_db!r}\n")
            for c in p.channels:
                f.write(f"{name}192 {c.id} {c.peak_db(192000)!r} {c.gain_db!r}\n")
    print("preamps:", st.preamp_db, mx.preamp_db, at.preamp_db)


if __name__ == "__main__":
    main()
