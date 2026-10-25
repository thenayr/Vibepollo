// SPDX-License-Identifier: GPL-3.0-or-later
#include "src/audio_bitstream.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <gtest/gtest.h>
#include <random>
#include <string>

namespace {
  using bytes = std::vector<std::uint8_t>;

  bytes round_trip(const bytes &raw, std::uint8_t flags = 0) {
    const auto packed = audio_bitstream::pack(raw, flags);
    EXPECT_TRUE(packed);
    bytes out;
    EXPECT_TRUE(packed && audio_bitstream::unpack(*packed, out));
    return out;
  }

  // One IEC 61937 burst: preamble, payload with some zero bytes mixed in, then
  // zero stuffing up to `period`.
  bytes make_burst(std::uint8_t type, std::size_t payload, std::size_t period, std::uint32_t seed) {
    bytes b(period, 0);
    std::copy(audio_bitstream::burst_sync.begin(), audio_bitstream::burst_sync.end(), b.begin());
    b[4] = type;
    b[6] = std::uint8_t(payload);
    b[7] = std::uint8_t(payload >> 8);
    std::mt19937 rng(seed);
    for (std::size_t i = 8; i < 8 + payload && i < period; ++i) {
      b[i] = (rng() % 5 == 0) ? 0 : std::uint8_t(1 + rng() % 255);
    }
    return b;
  }

  struct frame {
    bytes data;
    std::uint8_t flags;
  };

  std::vector<frame> frame_stream(const bytes &stream, std::size_t chunk) {
    audio_bitstream::framer framer;
    std::vector<frame> frames;
    auto emit = [&](std::span<const std::uint8_t> f, std::uint8_t flags) {
      frames.push_back({bytes(f.begin(), f.end()), flags});
    };
    for (std::size_t at = 0; at < stream.size(); at += chunk) {
      const auto n = std::min(chunk, stream.size() - at);
      framer.feed(std::span<const std::uint8_t>(stream.data() + at, n), emit);
    }
    framer.flush(emit);
    return frames;
  }

  bytes concat(const std::vector<frame> &frames) {
    bytes all;
    for (const auto &f : frames) all.insert(all.end(), f.data.begin(), f.data.end());
    return all;
  }
}  // namespace

TEST(AudioBitstream, PackRoundTripsEdgeCases) {
  EXPECT_EQ(round_trip({}), bytes {});

  const bytes silence(61440, 0);
  const auto packed_silence = audio_bitstream::pack(silence, 0);
  ASSERT_TRUE(packed_silence);
  EXPECT_EQ(packed_silence->size(), audio_bitstream::header_size);
  EXPECT_EQ(round_trip(silence), silence);

  const bytes dense(70000, 0xA5);  // Longer than one segment can hold.
  EXPECT_EQ(round_trip(dense), dense);
  EXPECT_EQ(audio_bitstream::parse_header(*audio_bitstream::pack(dense, 0))->segments, 2);

  bytes sparse(200000, 0);  // Zero runs longer than one skip field.
  sparse.front() = 1;
  sparse.back() = 2;
  EXPECT_EQ(round_trip(sparse), sparse);

  bytes trailing(10, 0);
  trailing.back() = 9;
  EXPECT_EQ(round_trip(trailing), trailing);
}

TEST(AudioBitstream, ShortZeroGapsStayInsideOneSegment) {
  const bytes three_zero_gap {1, 0, 0, 0, 2};
  const bytes four_zero_gap {1, 0, 0, 0, 0, 2};
  EXPECT_EQ(audio_bitstream::parse_header(*audio_bitstream::pack(three_zero_gap, 0))->segments, 1);
  EXPECT_EQ(audio_bitstream::parse_header(*audio_bitstream::pack(four_zero_gap, 0))->segments, 2);
  EXPECT_EQ(round_trip(three_zero_gap), three_zero_gap);
  EXPECT_EQ(round_trip(four_zero_gap), four_zero_gap);
}

TEST(AudioBitstream, ExactWireLayout) {
  const auto packed = audio_bitstream::pack(bytes {0, 0, 7, 8, 0, 0}, audio_bitstream::burst_aligned);
  ASSERT_TRUE(packed);
  const bytes expected {1, 1, 1, 0, 6, 0, 0, 0, 2, 0, 2, 0, 7, 8};
  EXPECT_EQ(*packed, expected);
}

TEST(AudioBitstream, PackRejectsOversizedFrames) {
  EXPECT_FALSE(audio_bitstream::pack(bytes(audio_bitstream::max_frame_bytes + 1, 1), 0));
  EXPECT_TRUE(audio_bitstream::pack(bytes(audio_bitstream::max_frame_bytes, 1), 0));
}

TEST(AudioBitstream, UnpackRejectsMalformedFramesWithoutTouchingOutput) {
  const bytes good {1, 0, 1, 0, 6, 0, 0, 0, 2, 0, 2, 0, 7, 8};
  const std::vector<bytes> bad {
    {},
    {1, 0, 1, 0, 6, 0, 0},  // Short header.
    {2, 0, 1, 0, 6, 0, 0, 0, 2, 0, 2, 0, 7, 8},  // Unknown version.
    {1, 0, 0, 0, 1, 0, 4, 0},  // Raw length above the limit.
    {1, 0, 1, 0, 6, 0, 0, 0, 5, 0, 2, 0, 7, 8},  // Segment runs past the raw length.
    {1, 0, 1, 0, 6, 0, 0, 0, 2, 0, 2, 0, 7},  // Truncated data.
    {1, 0, 1, 0, 6, 0, 0, 0, 2, 0},  // Truncated segment header.
    {1, 0, 2, 0, 6, 0, 0, 0, 2, 0, 2, 0, 7, 8},  // More segments than present.
    {1, 0, 1, 0, 6, 0, 0, 0, 2, 0, 2, 0, 7, 8, 9},  // Trailing bytes.
  };
  bytes out {42};
  ASSERT_TRUE(audio_bitstream::unpack(good, out));
  EXPECT_EQ(out, (bytes {42, 0, 0, 7, 8, 0, 0}));
  for (const auto &frame : bad) {
    bytes before = out;
    EXPECT_FALSE(audio_bitstream::unpack(frame, out));
    EXPECT_EQ(out, before);
  }
}

TEST(AudioBitstream, FramerCutsAtBurstPreamblesForAnyChunking) {
  bytes stream(1000, 0x33);  // Lead-in without sync, e.g. attaching mid-burst.
  for (std::uint32_t i = 0; i < 5; ++i) {
    const auto burst = make_burst(22, 4500, 61440, i);
    stream.insert(stream.end(), burst.begin(), burst.end());
  }
  for (const std::size_t chunk : {std::size_t(1), std::size_t(7), std::size_t(4096), stream.size()}) {
    const auto frames = frame_stream(stream, chunk);
    ASSERT_EQ(frames.size(), 6u) << "chunk " << chunk;
    EXPECT_EQ(frames[0].data.size(), 1000u);
    EXPECT_EQ(frames[0].flags, 0);
    for (std::size_t f = 1; f < frames.size(); ++f) {
      EXPECT_EQ(frames[f].data.size(), 61440u);
      EXPECT_EQ(frames[f].flags, audio_bitstream::burst_aligned);
    }
    EXPECT_EQ(concat(frames), stream);
  }
}

TEST(AudioBitstream, FramerIgnoresSyncPatternInsideABurstOnceThePeriodIsKnown) {
  bytes stream;
  for (std::uint32_t i = 0; i < 4; ++i) {
    auto burst = make_burst(22, 4500, 61440, i);
    if (i == 2) {
      std::copy(audio_bitstream::burst_sync.begin(), audio_bitstream::burst_sync.end(), burst.begin() + 2000);
    }
    stream.insert(stream.end(), burst.begin(), burst.end());
  }
  const auto frames = frame_stream(stream, 3000);
  ASSERT_EQ(frames.size(), 4u);
  for (const auto &f : frames) EXPECT_EQ(f.data.size(), 61440u);
  EXPECT_EQ(concat(frames), stream);
}

TEST(AudioBitstream, FramerResyncsWhenTheBurstPeriodChanges) {
  bytes stream;
  for (std::uint32_t i = 0; i < 3; ++i) {
    const auto burst = make_burst(22, 4500, 61440, i);
    stream.insert(stream.end(), burst.begin(), burst.end());
  }
  // AC-3's 6144-byte period divides MAT's 61440, so cutting at the old period
  // would land on an AC-3 preamble and merge ten bursts into one frame.
  for (std::uint32_t i = 0; i < 15; ++i) {
    const auto burst = make_burst(1, 1500, 6144, 10 + i);
    stream.insert(stream.end(), burst.begin(), burst.end());
  }
  const auto frames = frame_stream(stream, 5000);
  ASSERT_EQ(frames.size(), 18u);
  for (std::size_t f = 0; f < 3; ++f) EXPECT_EQ(frames[f].data.size(), 61440u);
  for (std::size_t f = 3; f < 18; ++f) EXPECT_EQ(frames[f].data.size(), 6144u);
  for (const auto &f : frames) EXPECT_EQ(f.flags, audio_bitstream::burst_aligned);
  EXPECT_EQ(concat(frames), stream);
}

TEST(AudioBitstream, FramerBoundsFramesWithoutSync) {
  const bytes stream(200000, 0x11);
  const auto frames = frame_stream(stream, 9999);
  ASSERT_EQ(frames.size(), 4u);
  for (std::size_t f = 0; f < 3; ++f) EXPECT_EQ(frames[f].data.size(), audio_bitstream::unaligned_frame_bytes);
  EXPECT_EQ(frames[3].data.size(), 200000u - 3 * audio_bitstream::unaligned_frame_bytes);
  EXPECT_EQ(concat(frames), stream);
}

// Round-trips a real capture from the probe driver's tap (tapreader's
// tap_<n>.wav). Set VIBEPOLLO_MAT_TAP_WAV to its path; skipped otherwise.
TEST(AudioBitstream, RealMatCaptureRoundTripsBitExact) {
  const char *path = std::getenv("VIBEPOLLO_MAT_TAP_WAV");
  if (!path || !*path) {
    GTEST_SKIP() << "VIBEPOLLO_MAT_TAP_WAV is not set";
  }
  std::ifstream file(path, std::ios::binary);
  ASSERT_TRUE(file) << path;
  const bytes wav((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());

  std::size_t at = 12;
  std::size_t data_at = 0;
  while (at + 8 <= wav.size()) {
    const std::uint32_t length = wav[at + 4] | (wav[at + 5] << 8) | (wav[at + 6] << 16) | (std::uint32_t(wav[at + 7]) << 24);
    if (std::equal(wav.begin() + at, wav.begin() + at + 4, "data")) {
      data_at = at + 8;
      break;
    }
    at += 8 + length + (length & 1);
  }
  ASSERT_NE(data_at, 0u) << "no data chunk";
  const std::span<const std::uint8_t> raw(wav.data() + data_at, wav.size() - data_at);

  audio_bitstream::framer framer;
  bytes rebuilt;
  rebuilt.reserve(raw.size());
  std::size_t frames = 0, aligned = 0, packed_total = 0, packed_aligned = 0, max_packed = 0;
  bool ok = true;
  auto emit = [&](std::span<const std::uint8_t> f, std::uint8_t flags) {
    const auto packed = audio_bitstream::pack(f, flags);
    if (!packed || !audio_bitstream::unpack(*packed, rebuilt)) {
      ok = false;
      return;
    }
    ++frames;
    packed_total += packed->size();
    max_packed = std::max(max_packed, packed->size());
    if (flags & audio_bitstream::burst_aligned) {
      ++aligned;
      packed_aligned += packed->size();
    }
  };
  // Uneven chunks, as reads from the tap ring arrive.
  std::mt19937 rng(1234);
  for (std::size_t pos = 0; pos < raw.size();) {
    const std::size_t n = std::min<std::size_t>(1 + rng() % 40000, raw.size() - pos);
    framer.feed(raw.subspan(pos, n), emit);
    pos += n;
  }
  const auto period = framer.period();
  framer.flush(emit);

  ASSERT_TRUE(ok);
  ASSERT_EQ(rebuilt.size(), raw.size());
  EXPECT_TRUE(std::equal(rebuilt.begin(), rebuilt.end(), raw.begin())) << "rebuilt stream differs";
  EXPECT_GT(aligned, 0u);
  EXPECT_EQ(period, 61440u);  // 20 ms of 8 ch / 192 kHz / 16-bit: one MAT burst.

  // 8 ch x 192 kHz x 16-bit container: 3,072,000 bytes per second.
  const double seconds = double(raw.size()) / 3072000.0;
  std::printf("[ MAT tap  ] %.1f s, %zu frames (%zu burst-aligned), period %zu bytes\n",
              seconds, frames, aligned, period);
  std::printf("[ MAT tap  ] packed %.2f MB of %.2f MB raw (%.1f:1), %.0f kbit/s; avg %.0f B per burst, max frame %zu B\n",
              packed_total / 1e6, raw.size() / 1e6, double(raw.size()) / double(packed_total),
              packed_total * 8.0 / seconds / 1000.0, aligned ? double(packed_aligned) / aligned : 0.0, max_packed);
}
