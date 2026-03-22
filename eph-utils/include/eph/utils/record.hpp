#pragma once

// Aggregation header — includes all record-related components for backward
// compatibility. Prefer including the specific header you need:
//   - eph/utils/hdr_histogram.hpp  (measure_tsc, ScopedTSC, HdrHistogram, Stats)
//   - eph/utils/system_stats.hpp   (SystemResourceStats, SystemStats)
//   - eph/utils/recorder.hpp       (Recorder, ConcurrentRecorder)

#include "eph/utils/hdr_histogram.hpp"
#include "eph/utils/recorder.hpp"
#include "eph/utils/system_stats.hpp"
