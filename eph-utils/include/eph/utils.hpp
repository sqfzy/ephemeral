#pragma once

/// @file utils.hpp
/// @brief Convenience header that includes all eph-utils public headers.
///
/// Include this single header to pull in every utility in the eph-utils
/// library. Prefer including individual headers when build times matter.

#include "eph/utils/alignment.hpp"
#include "eph/utils/audit_log.hpp"
#include "eph/utils/backoff.hpp"
#include "eph/utils/console_sink.hpp"
#include "eph/utils/cpu.hpp"
#include "eph/utils/ema.hpp"
#include "eph/utils/hdr_histogram.hpp"
#include "eph/utils/hugepage.hpp"
#include "eph/utils/kill_switch.hpp"
#include "eph/utils/phased_timer.hpp"
#include "eph/utils/rate_limiter.hpp"
#include "eph/utils/record.hpp"
#include "eph/utils/recorder.hpp"
#include "eph/utils/retry.hpp"
#include "eph/utils/scope_guard.hpp"
#include "eph/utils/shutdown_signal.hpp"
#include "eph/utils/system_stats.hpp"
#include "eph/utils/time.hpp"
#include "eph/utils/timestamp.hpp"
