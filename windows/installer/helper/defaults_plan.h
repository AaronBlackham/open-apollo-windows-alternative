/*
 * defaults.json: the driver registry values a release writes (apollo-helper regdefaults). Pure
 * parsing and validation, no registry access, so test_units.cpp checks it (and the release's own
 * defaults.json) offline at every build; sys.cpp writes what it returns.
 *
 *   { "overwrite": false,
 *     "values":  { "StripsEnable": 1,                                  number -> REG_DWORD
 *                  "Name": { "type": "dword", "data": "0xFF", "overwrite": true },
 *                  "Text": { "type": "sz", "data": "..." } },
 *     "subkeys": { "Models\\99": { "overwrite": true, "values": { "Mode": 2 } } } }
 *
 * "values" go to HKLM\SYSTEM\CurrentControlSet\Services\apollo\Parameters itself (the legacy,
 * service-wide values: the driver applies them to the x8p only). "subkeys" go below Parameters:
 * 1 to 3 parts of letters, digits and _, joined by single backslashes (driver\unit.c reads
 * Models\<TT>, TT = device type in hex, 99 = Twin X Gen 1). Keys starting with "_" are comments
 * at every level; any other unknown key is an error, so a typo never passes silently.
 * "overwrite": false keeps a value that is already set (an upgrade keeps the user's tuning);
 * a value's own "overwrite" wins over its subkey's, which wins over the top-level one.
 */
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <string>
#include <vector>

struct DefaultItem {
    std::wstring SubKey;            /* "" = Parameters itself, else e.g. "Models\\99" */
    std::wstring Name;
    DWORD        Type = REG_DWORD;  /* REG_DWORD or REG_SZ */
    DWORD        Dw = 0;
    std::wstring Sz;
    bool         Overwrite = false;
};

/* True and the items in file order (Parameters' own first, then each subkey), or false and why.
   A UTF-8 BOM is allowed. Nothing is returned on any error. */
bool ParseDefaults(const std::vector<BYTE> &data, std::vector<DefaultItem> &items, std::wstring &error);

/* "Models\\99"-style subkey path check (exposed for the test). */
bool ValidSubKeyPath(const std::wstring &path);
