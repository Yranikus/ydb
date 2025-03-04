// Copyright 2021 gRPC authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <grpc/support/port_platform.h>

#include "src/core/lib/gprpp/time.h"

#include <atomic>
#include <cstdint>
#include <limits>
#include <util/generic/string.h>
#include <util/string/cast.h>

#include <grpc/impl/codegen/gpr_types.h>
#include <grpc/support/log.h>

namespace grpc_core {

namespace {

std::atomic<int64_t> g_process_epoch_seconds;
std::atomic<gpr_cycle_counter> g_process_epoch_cycles;

GPR_ATTRIBUTE_NOINLINE std::pair<int64_t, gpr_cycle_counter> InitTime() {
  gpr_cycle_counter cycles_start = 0;
  gpr_cycle_counter cycles_end = 0;
  int64_t process_epoch_seconds = 0;

  // Check the current time... if we end up with zero, try again after 100ms.
  // If it doesn't advance after sleeping for 1100ms, crash the process.
  for (int i = 0; i < 11; i++) {
    cycles_start = gpr_get_cycle_counter();
    gpr_timespec now = gpr_now(GPR_CLOCK_MONOTONIC);
    cycles_end = gpr_get_cycle_counter();
    process_epoch_seconds = now.tv_sec - 1;
    if (process_epoch_seconds != 0) {
      break;
    }
    gpr_sleep_until(gpr_time_add(now, gpr_time_from_millis(100, GPR_TIMESPAN)));
  }

  // Time does not seem to be increasing from zero...
  GPR_ASSERT(process_epoch_seconds != 0);
  int64_t expected = 0;
  gpr_cycle_counter process_epoch_cycles = (cycles_start + cycles_end) / 2;
  GPR_ASSERT(process_epoch_cycles != 0);
  if (!g_process_epoch_seconds.compare_exchange_strong(
          expected, process_epoch_seconds, std::memory_order_relaxed,
          std::memory_order_relaxed)) {
    process_epoch_seconds = expected;
    do {
      process_epoch_cycles =
          g_process_epoch_cycles.load(std::memory_order_relaxed);
    } while (process_epoch_cycles == 0);
  } else {
    g_process_epoch_cycles.store(process_epoch_cycles,
                                 std::memory_order_relaxed);
  }
  return std::make_pair(process_epoch_seconds, process_epoch_cycles);
}

gpr_timespec StartTime() {
  int64_t sec = g_process_epoch_seconds.load(std::memory_order_relaxed);
  if (GPR_UNLIKELY(sec == 0)) sec = InitTime().first;
  return {sec, 0, GPR_CLOCK_MONOTONIC};
}

gpr_cycle_counter StartCycleCounter() {
  gpr_cycle_counter cycles =
      g_process_epoch_cycles.load(std::memory_order_relaxed);
  if (GPR_UNLIKELY(cycles == 0)) cycles = InitTime().second;
  return cycles;
}

gpr_timespec MillisecondsAsTimespec(int64_t millis, gpr_clock_type clock_type) {
  // special-case infinities as Timestamp can be 32bit on some
  // platforms while gpr_time_from_millis always takes an int64_t.
  if (millis == std::numeric_limits<int64_t>::max()) {
    return gpr_inf_future(clock_type);
  }
  if (millis == std::numeric_limits<int64_t>::min()) {
    return gpr_inf_past(clock_type);
  }

  if (clock_type == GPR_TIMESPAN) {
    return gpr_time_from_millis(millis, GPR_TIMESPAN);
  }
  return gpr_time_add(gpr_convert_clock_type(StartTime(), clock_type),
                      gpr_time_from_millis(millis, GPR_TIMESPAN));
}

int64_t TimespanToMillisRoundUp(gpr_timespec ts) {
  GPR_ASSERT(ts.clock_type == GPR_TIMESPAN);
  double x = GPR_MS_PER_SEC * static_cast<double>(ts.tv_sec) +
             static_cast<double>(ts.tv_nsec) / GPR_NS_PER_MS +
             static_cast<double>(GPR_NS_PER_SEC - 1) /
                 static_cast<double>(GPR_NS_PER_SEC);
  if (x <= static_cast<double>(std::numeric_limits<int64_t>::min())) {
    return std::numeric_limits<int64_t>::min();
  }
  if (x >= static_cast<double>(std::numeric_limits<int64_t>::max())) {
    return std::numeric_limits<int64_t>::max();
  }
  return static_cast<int64_t>(x);
}

int64_t TimespanToMillisRoundDown(gpr_timespec ts) {
  GPR_ASSERT(ts.clock_type == GPR_TIMESPAN);
  double x = GPR_MS_PER_SEC * static_cast<double>(ts.tv_sec) +
             static_cast<double>(ts.tv_nsec) / GPR_NS_PER_MS;
  if (x <= static_cast<double>(std::numeric_limits<int64_t>::min())) {
    return std::numeric_limits<int64_t>::min();
  }
  if (x >= static_cast<double>(std::numeric_limits<int64_t>::max())) {
    return std::numeric_limits<int64_t>::max();
  }
  return static_cast<int64_t>(x);
}

}  // namespace

Timestamp Timestamp::FromTimespecRoundUp(gpr_timespec ts) {
  return FromMillisecondsAfterProcessEpoch(TimespanToMillisRoundUp(gpr_time_sub(
      gpr_convert_clock_type(ts, GPR_CLOCK_MONOTONIC), StartTime())));
}

Timestamp Timestamp::FromTimespecRoundDown(gpr_timespec ts) {
  return FromMillisecondsAfterProcessEpoch(
      TimespanToMillisRoundDown(gpr_time_sub(
          gpr_convert_clock_type(ts, GPR_CLOCK_MONOTONIC), StartTime())));
}

Timestamp Timestamp::FromCycleCounterRoundUp(gpr_cycle_counter c) {
  return Timestamp::FromMillisecondsAfterProcessEpoch(
      TimespanToMillisRoundUp(gpr_cycle_counter_sub(c, StartCycleCounter())));
}

Timestamp Timestamp::FromCycleCounterRoundDown(gpr_cycle_counter c) {
  return Timestamp::FromMillisecondsAfterProcessEpoch(
      TimespanToMillisRoundDown(gpr_cycle_counter_sub(c, StartCycleCounter())));
}

gpr_timespec Timestamp::as_timespec(gpr_clock_type clock_type) const {
  return MillisecondsAsTimespec(millis_, clock_type);
}

TString Timestamp::ToString() const {
  return "@" + ::ToString(millis_) + "ms";
}

gpr_timespec Duration::as_timespec() const {
  return MillisecondsAsTimespec(millis_, GPR_TIMESPAN);
}

Duration Duration::FromTimespec(gpr_timespec t) {
  return Duration::Milliseconds(TimespanToMillisRoundUp(t));
}

TString Duration::ToString() const {
  return ::ToString(millis_) + "ms";
}

void TestOnlySetProcessEpoch(gpr_timespec epoch) {
  g_process_epoch_seconds.store(
      gpr_convert_clock_type(epoch, GPR_CLOCK_MONOTONIC).tv_sec);
}

std::ostream& operator<<(std::ostream& out, Timestamp timestamp) {
  return out << timestamp.ToString();
}

std::ostream& operator<<(std::ostream& out, Duration duration) {
  return out << duration.ToString();
}

}  // namespace grpc_core
