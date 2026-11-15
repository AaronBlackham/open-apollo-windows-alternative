/*
 * apollo_acx.h - ACX (Audio Class eXtension) headers for apollo.sys.
 *
 * Include after apollo.h. Order matters in C: ks.h needs windef.h, ksmedia.h
 * needs mmsystem.h (docs/PHASE2.md section 7). ACX_VERSION_MAJOR/MINOR come
 * from the project (1.1); acxstub.lib binds the driver to Acx01000.sys.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */
#pragma once

#include <windef.h>
#include <mmsystem.h>
#include <ks.h>
#include <ksmedia.h>
#include <acx.h>
