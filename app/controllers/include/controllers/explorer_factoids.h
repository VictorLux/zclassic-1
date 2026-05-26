/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Explorer factoids — historian nerd stats with data receipts.
 *
 * Compatibility shim: the page-assembly logic moved to the view shape
 * (app/views/src/explorer_factoids_view.c) per checklist item D2
 * ("controllers must not build views"). This header forwards to the
 * view header so existing controller includes keep compiling. */

#ifndef ZCL_CONTROLLERS_EXPLORER_FACTOIDS_H
#define ZCL_CONTROLLERS_EXPLORER_FACTOIDS_H

#include "views/explorer_factoids_view.h"

#endif
