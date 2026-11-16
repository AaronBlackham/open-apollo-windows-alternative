/*
 * aggjson-test.cpp - offline checks of app/src/agg_json.h (gate G6): Apollo Control's Units & routing
 * JSON (docs/apollo-control-units.md section 2) to the driver's APOLLO_AGG_CONFIG and back, and the
 * layout the driver would compile from it (driver/apollo_agg.h). Exit code 0 = all passed.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include <windows.h>
#include <stdio.h>
#include "..\..\include\apollo_ioctl.h"
#include "..\..\driver\apollo_agg.h"
#include "..\..\app\src\agg_json.h"

static int g_fail, g_pass;
static void Check(bool ok, const char *what)
{
    if (ok) g_pass++;
    else { g_fail++; printf("FAIL: %s\n", what); }
}

static APOLLO_AGG_CONFIG g_c, g_c2;

int main()
{
    std::string err;
    /* the example of docs/apollo-control-units.md section 2, with this PC's unit keys */
    const std::string doc =
        "{\"version\":1,\"units\":["
        "{\"id\":\"000000002F00BEEF\",\"model\":\"Apollo Twin X\",\"deviceType\":153,\"serial\":\"20000000000002\"},"
        "{\"id\":\"000000003A00C0DE\",\"model\":\"Apollo x8p\",\"deviceType\":34,\"serial\":\"20000000000001\"}],"
        "\"master\":\"000000002F00BEEF\",\"channels\":null,\"preset\":\"studio\",\"routes\":["
        "{\"src\":\"daw\",\"unit\":\"000000002F00BEEF\",\"port\":\"mon\",\"ids\":[2305,2306]},"
        "{\"src\":\"windows\",\"unit\":\"000000002F00BEEF\",\"port\":\"mon\",\"ids\":[2305,2306]},"
        "{\"src\":\"000000002F00BEEF:line3-4\",\"unit\":\"000000002F00BEEF\",\"port\":\"line3-4\",\"ids\":[2563,2564]},"
        "{\"src\":\"000000003A00C0DE:mon\",\"unit\":\"000000003A00C0DE\",\"port\":\"mon\",\"ids\":[2305,2306]}],"
        "\"futureKey\":{\"kept\":[1,2,3]}}";

    Check(aggjson::AggCompile(doc, g_c, err), "the documented example compiles");
    if (!err.empty()) printf("  (%s)\n", err.c_str());
    Check(ApolloAggValidate(&g_c) == 0, "and passes the driver's checks");
    Check(g_c.UnitCount == 2 && g_c.Unit[0].Uid == 0x2F00BEEFull && g_c.Unit[0].Model == APOLLO_INFO_MODEL_TWINX &&
          g_c.Unit[1].Model == APOLLO_INFO_MODEL_X8P && g_c.Master == 0, "units, models, master");
    Check(!(g_c.Flags & APOLLO_AGG_CFG_EXPLICIT) && (g_c.Flags & APOLLO_AGG_CFG_ENABLED) && g_c.AsrcHalf == 16,
          "simple order, enabled, default ASRC");
    Check(g_c.RouteCount == 4 && g_c.Route[0].SrcKind == APOLLO_AGG_SRC_DAW && g_c.Route[0].DstId[0] == 0x0901 &&
          g_c.Route[1].SrcKind == APOLLO_AGG_SRC_WINDOWS && g_c.Route[2].SrcKind == APOLLO_AGG_SRC_PORT &&
          g_c.Route[2].SrcUnit == 0 && g_c.Route[2].SrcId[0] == 0x0A03 && g_c.Route[3].SrcUnit == 1 &&
          g_c.Route[3].SrcId[0] == 0x0901 && g_c.Route[3].DstUnit == 1, "routes");
    Check(g_c.JsonBytes == doc.size() && memcmp(g_c.Json, doc.data(), doc.size()) == 0, "the JSON kept verbatim");

    /* the message wrapper */
    std::string msg = "{\"cmd\":\"agg-set\",\"config\":" + doc + "}";
    Check(aggjson::ConfigTextOf(msg) == doc, "config text out of the agg-set message");

    /* explicit order + hidden, ASRC quality */
    std::string adv =
        "{\"units\":[{\"id\":\"000000003A00C0DE\",\"deviceType\":34},{\"id\":\"000000002F00BEEF\",\"deviceType\":153}],"
        "\"master\":\"000000003A00C0DE\",\"asrcQuality\":\"high\",\"routes\":[],"
        "\"channels\":{\"in\":[{\"unit\":\"000000002F00BEEF\",\"slot\":0,\"hidden\":false},"
        "{\"unit\":\"000000003A00C0DE\",\"slot\":3,\"hidden\":true}],\"out\":[{\"unit\":\"000000002F00BEEF\",\"slot\":1}]}}";
    Check(aggjson::AggCompile(adv, g_c, err), "advanced config compiles");
    Check((g_c.Flags & APOLLO_AGG_CFG_EXPLICIT) && g_c.InCount == 2 && g_c.In[0].Unit == 1 && g_c.In[1].Unit == 0 &&
          g_c.In[1].Slot == 3 && (g_c.In[1].Flags & APOLLO_AGG_CHAN_HIDDEN) && g_c.OutCount == 1 && g_c.AsrcHalf == 32 &&
          g_c.Master == 0, "explicit lists, hidden, high ASRC, x8p master");

    /* describe -> compile round trip (a driver config without JSON) */
    aggjson::AggCompile(doc, g_c, err);
    g_c.JsonBytes = 0;
    std::map<std::string, aggjson::UnitText> known;
    known["000000002F00BEEF"] = { "Apollo Twin X (Gen 1)", "20000000000002" };
    std::string d = aggjson::AggDescribe(g_c, known);
    printf("describe: %s\n", d.c_str());
    Check(aggjson::AggCompile(d, g_c2, err), "the description compiles again");
    Check(g_c2.UnitCount == g_c.UnitCount && g_c2.Master == g_c.Master && g_c2.RouteCount == g_c.RouteCount &&
          memcmp(g_c2.Route, g_c.Route, sizeof(g_c.Route)) == 0 && g_c2.Unit[0].Uid == g_c.Unit[0].Uid,
          "round trip keeps units, master and routes");
    Check(d.find("\"preset\":\"custom\"") != std::string::npos, "preset label from the routes");
    aggjson::AggCompile(adv, g_c, err);
    g_c.JsonBytes = 0;
    d = aggjson::AggDescribe(g_c, known);
    Check(aggjson::AggCompile(d, g_c2, err) && (g_c2.Flags & APOLLO_AGG_CFG_EXPLICIT) && g_c2.InCount == 2 &&
          (g_c2.In[1].Flags & APOLLO_AGG_CHAN_HIDDEN) && g_c2.AsrcHalf == 32, "round trip of the explicit order");

    /* the layout the driver compiles from the Studio default (x8p first, Twin X master) */
    {
        std::string studio = "{\"units\":[{\"id\":\"000000003A00C0DE\",\"deviceType\":34},{\"id\":\"000000002F00BEEF\","
                             "\"deviceType\":153}],\"master\":\"000000002F00BEEF\",\"channels\":null,\"preset\":\"studio\","
                             "\"routes\":[{\"src\":\"daw\",\"unit\":\"000000002F00BEEF\",\"port\":\"mon\"}]}";
        Check(aggjson::AggCompile(studio, g_c, err), "studio without ids (port only) compiles");
        Check(g_c.Route[0].DstId[0] == 0x0901 && g_c.Route[0].DstId[1] == 0x0902, "port mon -> 0x0901/0x0902");
    }

    /* [G7] "align": absent or true = aligned (as in G6), false = lowest latency; described back */
    Check(aggjson::AggCompile(doc, g_c, err) && (g_c.Flags & APOLLO_AGG_CFG_ALIGN), "no align key: aligned, as before");
    {
        std::string un = "{\"units\":[{\"id\":\"000000003A00C0DE\",\"deviceType\":34}],\"master\":\"000000003A00C0DE\","
                         "\"align\":false}";
        Check(aggjson::AggCompile(un, g_c, err) && !(g_c.Flags & APOLLO_AGG_CFG_ALIGN), "align false: unaligned");
        g_c.JsonBytes = 0;
        d = aggjson::AggDescribe(g_c, known);
        Check(d.find("\"align\":false") != std::string::npos && aggjson::AggCompile(d, g_c2, err) &&
              !(g_c2.Flags & APOLLO_AGG_CFG_ALIGN), "align round trip");
        aggjson::AggCompile(doc, g_c, err);
        g_c.JsonBytes = 0;
        d = aggjson::AggDescribe(g_c, known);
        Check(d.find("\"align\":true") != std::string::npos, "aligned described as align true");
    }

    /* refusals */
    Check(!aggjson::AggCompile("[1,2]", g_c, err), "not an object");
    Check(!aggjson::AggCompile("{\"units\":[],\"master\":\"x\"}", g_c, err), "no units");
    Check(!aggjson::AggCompile("{\"units\":[{\"id\":\"SN20000000000001\"}],\"master\":\"SN20000000000001\"}", g_c, err),
          "a unit without HW_UID key is refused");
    Check(!aggjson::AggCompile("{\"units\":[{\"id\":\"000000003A00C0DE\"}],\"master\":\"000000002F00BEEF\"}", g_c, err),
          "master not a unit");
    Check(!aggjson::AggCompile("{\"units\":[{\"id\":\"000000003A00C0DE\"}],\"master\":\"000000003A00C0DE\","
                               "\"routes\":[{\"src\":\"bogus\",\"unit\":\"000000003A00C0DE\",\"port\":\"mon\"}]}", g_c, err),
          "unknown route source");
    Check(!aggjson::AggCompile("{\"units\":[{\"id\":\"000000003A00C0DE\"}],\"master\":\"000000003A00C0DE\"", g_c, err),
          "truncated JSON");
    Check(!aggjson::AggCompile("{\"a\":\"\\uZZZZ\"}", g_c, err), "bad escape");

    printf("aggjson-test: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
