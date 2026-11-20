// Levels & module registry — src/core/level_filter.cpp.
// Also implements the C functions tblog_module_get, tblog_set_level,
// tblog_get_level from log.h.
#pragma once

#include "tblog/log.h"

namespace tblog {

const char *level_name(tblog_level lv);              // "TRACE".."FATAL","OFF"
bool parse_level(const char *s, tblog_level *out);   // case-insensitive
void reset_levels_for_test();                        // root=INFO, clear explicit

}  // namespace tblog
