/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Explorer stats — comprehensive blockchain statistics.
 *
 * Compatibility shim: the page-assembly logic moved to the view shape
 * (app/views/src/explorer_stats_view.c) per checklist item D2
 * ("controllers must not build views"). This header forwards to the
 * view header so existing controller includes keep compiling. */

#ifndef ZCL_CONTROLLERS_EXPLORER_STATS_H
#define ZCL_CONTROLLERS_EXPLORER_STATS_H

#include "views/explorer_stats_view.h"

#endif
