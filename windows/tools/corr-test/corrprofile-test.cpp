/*
 * corrprofile-test.cpp - offline checks of app\src\corr_profile.h, Apollo Control's host side of
 * speaker correction:
 *
 *   1. its port of validate() gives exactly Avium Measure's answers (ref\cases.json: every problem,
 *      the same text, the same order, for the good profile and 47 broken ones, made by Avium
 *      Measure's own validate() in ref\make-ref.py)
 *   2. its designs (dsp\biquad.py design()) reproduce the coefficients Avium Measure wrote, every type
 *      and every rate, and a file with only the 48 kHz set compiles at 96 kHz to Avium Measure's 96 kHz set
 *   3. what it compiles for the driver passes the driver's own check (driver\corr_dsp.h CorrValidate) at
 *      every rate, and the compile refuses what the driver could not run (a delay over 10 ms, no
 *      output chosen, two channels on one slot, more than four channels)
 *   4. Python's repr for the messages, and the state JSON the page reads
 *
 * Usage: corrprofile-test.exe [ref folder]. Exit 0 = every check passed.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "..\..\app\src\corr_profile.h"
#include "..\..\driver\corr_dsp.h"

static int g_pass, g_fail;

static void Check(bool ok, const std::string &what)
{
    if (ok) {
        g_pass++;
    } else {
        g_fail++;
        printf("FAIL  %s\n", what.c_str());
    }
}

static std::string ReadFile(const std::string &p)
{
    std::ifstream f(p, std::ios::binary);
    std::stringstream s;
    s << f.rdbuf();
    return s.str();
}

static std::string Join(const std::vector<std::string> &v)
{
    std::string o;
    for (const auto &s : v) o += (o.empty() ? "" : " | ") + s;
    return o.empty() ? "(none)" : o;
}

static bool Load(const std::string &path, corrprof::J &out)
{
    std::string err;
    return corrprof::ParseJson(ReadFile(path), out, err);
}

int main(int argc, char **argv)
{
    std::string ref = argc > 1 ? argv[1] : "ref";
    using namespace corrprof;

    /* ---- 1. validate(): Avium Measure's verdicts */
    {
        J cases;
        std::string err;
        bool ok = ParseJson(ReadFile(ref + "\\cases.json"), cases, err);
        Check(ok && cases.kind == J::Arr && cases.a.size() >= 40, "ref\\cases.json reads (" + err + ")");
        int n = 0;
        for (const J &c : cases.a) {
            const J *name = c.Get("name"), *text = c.Get("text"), *want = c.Get("problems");
            if (!name || !text || !want || want->kind != J::Arr) {
                Check(false, "a case without name/text/problems");
                continue;
            }
            J d;
            std::string perr;
            if (!ParseJson(text->s, d, perr)) {
                Check(false, name->s + ": the profile text does not parse (" + perr + ")");
                continue;
            }
            std::vector<std::string> got = Validate(d), exp;
            for (const J &p : want->a) exp.push_back(p.s);
            Check(got == exp, "validate() port, " + name->s + ":\n        want " + Join(exp) + "\n        got  " + Join(got));
            n++;
        }
        printf("      validate() port: %d profiles, answers compared word for word with Avium Measure's\n", n);
    }

    /* ---- 2. designs: every filter of the reference files at every rate */
    {
        double worst = 0;
        int sets = 0;
        for (const char *file : { "studio.json", "max20.json", "alltypes.json" }) {
            J d;
            Check(Load(ref + "\\" + file, d), std::string(file) + " reads");
            Check(Validate(d).empty(), std::string(file) + " passes the port of validate()");
            for (const J &c : d.Get("channels")->a) {
                for (const J &b : c.Get("filters")->a) {
                    const TypeInfo *t = TypeOf(b.Get("type"));
                    double g = 0, q = BUTTERWORTH_Q;
                    AsFloat(b.Get("gain_db"), 0.0, g);
                    AsFloat(b.Get("q"), BUTTERWORTH_Q, q);
                    for (const auto &kv : b.Get("coefficients")->o) {
                        auto w = Design(*t, b.Get("freq_hz")->n, g, q, atof(kv.first.c_str()));
                        for (int k = 0; k < 5; k++) {
                            double e = std::fabs(w[(size_t)k] - kv.second.a[(size_t)k].n) / std::max(1.0, std::fabs(w[(size_t)k]));
                            worst = std::max(worst, e);
                        }
                        sets++;
                    }
                }
            }
        }
        char m[160];
        snprintf(m, sizeof(m), "designs = Avium Measure's coefficients: %d sets (12 types, 6 rates), worst %.2g", sets, worst);
        Check(worst < 1e-13, m);
        printf("      %s\n", m);
    }

    /* ---- 3. compile for the driver */
    {
        J d;
        Load(ref + "\\studio.json", d);
        std::vector<SlotPick> lr = { { "L", 0 }, { "R", 1 } };
        for (unsigned r : RATES) {
            APOLLO_CORR_PROFILE p;
            std::string err;
            unsigned long ch, fl;
            bool ok = Compile(d, r, lr, true, p, err);
            unsigned long e = ok ? CorrValidate(&p, 34, &ch, &fl) : 99;
            Check(ok && e == 0, "studio compiled at " + std::to_string(r) + " Hz passes the driver's check (" + err + ", " +
                  std::to_string(e) + ")");
            if (r == 48000 && ok) {
                Check(p.ChannelCount == 2 && p.Channel[0].Slot == 0 && p.Channel[1].Slot == 1 && p.Channel[0].FilterCount == 7 &&
                      p.Channel[0].Filter[6].Enabled == 0 && p.Channel[1].DelayUs == 420 && std::fabs(p.PreampDb + 2.5) < 1e-12 &&
                      std::fabs(p.Preamp - std::pow(10.0, -2.5 / 20)) < 1e-15 && strcmp(p.Name, "Studio A, listening position") == 0 &&
                      (p.Flags & APOLLO_CORR_LOAD_ENABLE) != 0,
                      "studio at 48 kHz: slots 0/1, 7 filters on L (one disabled), R delay 420 us, preamp -2.5 dB, name, enable");
                const J &c0 = d.Get("channels")->a[0].Get("filters")->a[0];
                bool same = true;
                for (int k = 0; k < 5; k++) same = same && p.Channel[0].Filter[0].Coef[k] == c0.Get("coefficients")->Get("48000")->a[(size_t)k].n;
                Check(same, "the file's 48 kHz coefficients go to the driver unchanged");
                Check(std::string(p.Created) == "2026-11-15T02:10:00Z" &&
                      std::string(p.Summary) == "Standard, 9 spots, loopback timing. Target Studio tilt, -0.8 dB/oct around 1 kHz. "
                                                "Boosts up to 6 dB.",
                      std::string("created and the measurement summary go along: ") + p.Summary);
                std::string pj = ProfileJson(p), perr;
                J back;
                Check(ParseJson(pj, back, perr) && back.Get("channels") && back.Get("channels")->a.size() == 2 &&
                      back.Get("channels")->a[0].Get("filters")->a.size() == 7, "the state JSON for the page parses back");
            }
        }
        {
            /* only the 48 kHz set in the file: 96 kHz is designed from the parameters, as Avium Measure would */
            J full, only48;
            Load(ref + "\\studio.json", full);
            Load(ref + "\\studio.json", only48);
            for (auto &kv : only48.o) {
                if (kv.first != "channels") continue;
                for (J &c : kv.second.a) {
                    for (auto &ckv : c.o) {
                        if (ckv.first != "filters") continue;
                        for (J &b : ckv.second.a) {
                            for (auto &bkv : b.o) {
                                if (bkv.first != "coefficients") continue;
                                std::vector<std::pair<std::string, J>> keep;
                                for (auto &r : bkv.second.o) if (r.first == "48000") keep.push_back(r);
                                bkv.second.o = keep;
                            }
                        }
                    }
                }
            }
            Check(Validate(only48).empty(), "a profile with only the 48 kHz coefficients passes (the format allows it)");
            APOLLO_CORR_PROFILE a, b;
            std::string e1, e2;
            Compile(full, 96000, lr, false, a, e1);
            Compile(only48, 96000, lr, false, b, e2);
            double worst = 0;
            for (unsigned long c = 0; c < 2; c++) {
                for (unsigned long k = 0; k < a.Channel[c].FilterCount; k++) {
                    for (int i = 0; i < 5; i++) {
                        worst = std::max(worst, std::fabs(a.Channel[c].Filter[k].Coef[i] - b.Channel[c].Filter[k].Coef[i]));
                    }
                }
            }
            char m[160];
            snprintf(m, sizeof(m), "no 96 kHz set in the file: designed, %.2g from Avium Measure's own 96 kHz set", worst);
            Check(worst < 1e-13, m);
        }
        {
            APOLLO_CORR_PROFILE p;
            std::string err;
            J dd;
            Load(ref + "\\studio.json", dd);
            Check(!Compile(dd, 48000, { { "L", -1 }, { "R", -1 } }, false, p, err) && !err.empty(), "no output chosen: refused (" + err + ")");
            Check(!Compile(dd, 48000, { { "L", 2 }, { "R", 2 } }, false, p, err), "L and R on one slot: refused (" + err + ")");
            Check(!Compile(dd, 48000, { { "L", 64 }, { "R", 1 } }, false, p, err), "slot 64: refused (" + err + ")");
            Check(!Compile(dd, 12345, lr, false, p, err), "a rate the format has no coefficients for: refused (" + err + ")");
            Check(Compile(dd, 48000, { { "L", 0 }, { "R", -1 } }, false, p, err) && p.ChannelCount == 1,
                  "only L chosen: one channel goes to the driver");
            for (auto &kv : dd.o) {
                if (kv.first != "channels") continue;
                for (auto &ckv : kv.second.a[1].o) {
                    if (ckv.first == "delay_ms") ckv.second.n = 25.0;
                }
            }
            Check(Validate(dd).empty(), "a 25 ms delay is a valid profile (the format allows 100 ms)");
            Check(!Compile(dd, 48000, lr, false, p, err) && err.find("10 ms") != std::string::npos,
                  "... but more than the driver's 10 ms ring: refused (" + err + ")");
            J m4;
            Load(ref + "\\studio.json", m4);
            for (auto &kv : m4.o) {
                if (kv.first != "channels") continue;
                J l = kv.second.a[0];
                const char *ids[] = { "C", "SW", "LS" };
                for (const char *id : ids) {
                    J c = l;
                    for (auto &ckv : c.o) if (ckv.first == "id") ckv.second.s = id;
                    kv.second.a.push_back(c);
                }
            }
            Check(Validate(m4).empty(), "five channels are a valid profile");
            Check(!Compile(m4, 48000, { { "L", 0 }, { "R", 1 }, { "C", 2 }, { "SW", 3 }, { "LS", 4 } }, false, p, err),
                  "five channels on outputs: more than the driver's four, refused (" + err + ")");
        }
    }

    /* ---- 3b. the driver's resonance limit admits everything the format can describe */
    {
        int n = 0, bad = 0;
        double worst = 0;
        for (const auto &t : TYPES) {
            for (double fs : { 44100.0, 48000.0, 96000.0, 192000.0 }) {
                for (double f : { 1.0, 20.0, 1000.0, 15000.0, 0.49 * fs, 0.6 * fs }) {
                    for (double g : { -30.0, -6.0, 0.0, 6.0, 30.0 }) {
                        for (double q : { 0.05, 0.7071, 10.0, 50.0 }) {
                            auto c = Design(t, f, g, q, fs);
                            n++;
                            if (!CorrStable(c[3], c[4])) continue;     /* (a 1 Hz q 50 pole at 192 kHz: past 1 - 1e-9) */
                            double w = (1 + c[3] + c[4]) * (1 - c[3] + c[4]), d = 1 - c[4];
                            worst = std::max(worst, std::sqrt(std::max(w, 0.0)) / (2 * d));
                            if (!CorrPoleQOk(c[3], c[4])) bad++;
                        }
                    }
                }
            }
        }
        char m[200];
        snprintf(m, sizeof(m), "every format design (%d: 12 types, q 0.05-50, +-30 dB, 1 Hz to 0.6 fs) is within the pole Q limit "
                 "(largest %.2f <= 282), %d refused", n, worst, bad);
        Check(bad == 0 && worst > 281.0, m);
        printf("      %s\n", m);
        for (const char *file : { "max20.json", "alltypes.json" }) {
            J d;
            Load(ref + "\\" + file, d);
            std::vector<SlotPick> picks = { { "L", 0 }, { "R", 1 } };
            for (unsigned r : RATES) {
                APOLLO_CORR_PROFILE p;
                std::string err;
                unsigned long ch, fl;
                bool ok = Compile(d, r, picks, true, p, err);
                unsigned long e = ok ? CorrValidate(&p, 34, &ch, &fl) : 99;
                Check(ok && e == 0, std::string(file) + " compiled at " + std::to_string(r) + " Hz passes the driver's check (" +
                      std::to_string(e) + ")");
            }
        }
    }

    /* ---- 4. repr and state */
    {
        struct { double v; const char *want; } fr[] = {
            { 1.0, "1.0" }, { 0.1, "0.1" }, { 1e16, "1e+16" }, { 1.5e-5, "1.5e-05" }, { 0.0001, "0.0001" },
            { 123.456, "123.456" }, { -0.0, "-0.0" }, { 1234567890123456.0, "1234567890123456.0" }, { 1e22, "1e+22" },
            { -2.5, "-2.5" }, { 2.0 / 3.0, "0.6666666666666666" },
        };
        for (const auto &f : fr) {
            Check(FloatRepr(f.v) == f.want, "repr(" + std::string(f.want) + ") = " + FloatRepr(f.v));
        }
        J s;
        s.kind = J::Str;
        s.s = "it's";
        Check(Repr(&s) == "\"it's\"" && Repr(nullptr) == "None", "repr of strings and None");
        APOLLO_CORR_STATE st{};
        st.Flags = APOLLO_CORR_F_LOADED | APOLLO_CORR_F_ENABLED | APOLLO_CORR_F_RUNNING;
        st.WetPermille = 1000;
        J back;
        std::string err;
        Check(ParseJson(StateJson(st), back, err) && back.Get("enabled")->b && back.Get("profile")->kind == J::Null,
              "the state JSON parses, no profile = null");
    }

    printf("corrprofile-test: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
