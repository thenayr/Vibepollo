/**
 * @file tests/unit/test_atmos_tap_live.cpp
 * @brief Captures from the real Atmos probe tap. Needs the probe driver,
 *        an elevated prompt, and audio playing to the probe endpoint.
 *
 * Set VIBEPOLLO_LIVE_TAP_SECONDS to how long to capture; skipped otherwise.
 * The endpoint test runs when VIBEPOLLO_LIVE_TAP_ENDPOINT is set, and needs
 * only the probe driver installed.
 */
#include "src/platform/windows/atmos_tap_capture.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <gtest/gtest.h>
#include <mutex>
#include <thread>

using namespace platf::audio::atmos_tap;

TEST(AtmosTapLive, FindsTheProbeEndpoint) {
  const char *enabled = std::getenv("VIBEPOLLO_LIVE_TAP_ENDPOINT");
  if (!enabled || !*enabled) {
    GTEST_SKIP() << "VIBEPOLLO_LIVE_TAP_ENDPOINT is not set";
  }
  EXPECT_TRUE(endpoint_present()) << "No active render endpoint named " << endpoint_name;
}

TEST(AtmosTapLive, CapturesContiguousMatFramesFromTheDriver) {
  const char *seconds_text = std::getenv("VIBEPOLLO_LIVE_TAP_SECONDS");
  if (!seconds_text || !*seconds_text) {
    GTEST_SKIP() << "VIBEPOLLO_LIVE_TAP_SECONDS is not set";
  }
  const int seconds = std::atoi(seconds_text);

  std::string error;
  auto capture = capture_t::open(error);
  ASSERT_TRUE(capture) << error;

  using clock = std::chrono::steady_clock;
  std::mutex lock;
  std::vector<stream_info_t> streams;
  std::size_t frames = 0, aligned = 0, discontinuities = 0, packed = 0, bad = 0;
  std::uint64_t raw = 0;
  std::uint32_t period = 0;
  std::size_t period_changes = 0;
  double max_gap_ms = 0, gap_sum_ms = 0;
  std::size_t gaps = 0;
  clock::time_point last_frame {};
  std::vector<std::uint8_t> scratch;

  capture_t::callbacks_t callbacks;
  callbacks.stream_started = [&](const stream_info_t &info) {
    std::lock_guard g {lock};
    streams.push_back(info);
    period = 0;
    last_frame = {};
  };
  callbacks.frame = [&](packed_frame_t &&f) {
    const auto now = clock::now();
    std::lock_guard g {lock};
    scratch.clear();
    if (!audio_bitstream::unpack(f.bytes, scratch) || scratch.size() != f.raw_length) ++bad;
    ++frames;
    packed += f.bytes.size();
    raw += f.raw_length;
    if (f.flags & audio_bitstream::discontinuity) ++discontinuities;
    if (f.flags & audio_bitstream::burst_aligned) {
      ++aligned;
      if (period && f.raw_length != period) ++period_changes;
      period = f.raw_length;
    }
    if (last_frame != clock::time_point {}) {
      const double gap = std::chrono::duration<double, std::milli>(now - last_frame).count();
      max_gap_ms = std::max(max_gap_ms, gap);
      gap_sum_ms += gap;
      ++gaps;
    }
    last_frame = now;
  };
  ASSERT_TRUE(capture->start(std::move(callbacks)));
  std::printf("[ live tap ] capturing for %d s; play audio to the probe now\n", seconds);
  std::this_thread::sleep_for(std::chrono::seconds(seconds));
  capture->stop();

  std::lock_guard g {lock};
  for (const auto &s : streams) {
    std::printf("[ live tap ] stream %d: tag 0x%04X, %u ch, %u Hz, %u-bit, encoding %d%s\n",
                s.serial, s.format.format_tag, s.format.channels, s.format.sample_rate,
                s.format.bits_per_sample, int(s.format.encoding), s.from_start ? "" : " (joined mid-stream)");
  }
  const double secs = double(raw) / 3072000.0;
  std::printf("[ live tap ] %zu frames (%zu burst-aligned), %.1f s of 8ch/192k, packed %.0f kbit/s\n",
              frames, aligned, secs, secs > 0 ? packed * 8.0 / secs / 1000.0 : 0.0);
  std::printf("[ live tap ] frame spacing: avg %.1f ms, max %.1f ms; lost %llu bytes; discontinuities %zu\n",
              gaps ? gap_sum_ms / gaps : 0.0, max_gap_ms, (unsigned long long) capture->lost_bytes(), discontinuities);

  ASSERT_FALSE(streams.empty()) << "Nothing played to the probe during the capture";
  EXPECT_GT(aligned, 0u) << "No IEC 61937 bursts: is Dolby Atmos for home theater on for the probe?";
  EXPECT_EQ(bad, 0u);
  EXPECT_EQ(capture->lost_bytes(), 0u);
  // Stopping mid-burst flushes a short last frame; nothing else may change the period.
  EXPECT_LE(period_changes, streams.size());
  EXPECT_EQ(discontinuities, streams.size());
}
