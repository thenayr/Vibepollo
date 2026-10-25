// SPDX-License-Identifier: GPL-3.0-or-later
#include "src/audio_bitstream_protocol.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <gtest/gtest.h>
#include <random>

namespace {
  using namespace audio_bitstream::protocol;
  using bytes = std::vector<std::uint8_t>;

  std::vector<bytes> chunks_of(const bytes &frame, std::uint8_t flags, std::uint16_t id) {
    std::vector<bytes> chunks;
    EXPECT_TRUE(chunk_frame(frame, flags, id, [&](std::span<const std::uint8_t, CHUNK_BYTES> c) {
      chunks.emplace_back(c.begin(), c.end());
    }));
    return chunks;
  }

  struct received {
    bytes data;
    std::uint8_t flags;
  };

  bytes pattern(std::size_t n, std::uint32_t seed) {
    bytes b(n);
    std::mt19937 rng(seed);
    for (auto &v : b) v = std::uint8_t(rng());
    return b;
  }
}  // namespace

TEST(AudioBitstreamProtocol, ChunksAreFixedSizeAndCoverTheFrame) {
  for (const std::size_t n : {std::size_t(8), CHUNK_DATA_BYTES, CHUNK_DATA_BYTES + 1, std::size_t(4946)}) {
    const auto frame = pattern(n, std::uint32_t(n));
    const auto chunks = chunks_of(frame, audio_bitstream::burst_aligned, 7);
    ASSERT_EQ(chunks.size(), chunks_for(n)) << n;
    for (std::size_t i = 0; i < chunks.size(); ++i) {
      EXPECT_EQ(chunks[i].size(), CHUNK_BYTES);
      const auto h = parse_chunk(chunks[i]);
      ASSERT_TRUE(h);
      EXPECT_EQ(h->index, i);
      EXPECT_EQ(h->count, chunks.size());
      EXPECT_EQ(h->frame_length, n);
      EXPECT_EQ(h->frame_id, 7);
      EXPECT_EQ(h->flags, audio_bitstream::burst_aligned);
    }
  }
  EXPECT_EQ(chunks_for(CHUNK_DATA_BYTES), 1u);
  EXPECT_EQ(chunks_for(CHUNK_DATA_BYTES + 1), 2u);
  EXPECT_FALSE(chunk_frame(bytes(MAX_FRAME_BYTES + 1), 0, 0, [](auto) {}));
}

TEST(AudioBitstreamProtocol, ExactChunkHeaderLayout) {
  const auto chunks = chunks_of(bytes {9, 8, 7}, 0x03, 0x1234);
  ASSERT_EQ(chunks.size(), 1u);
  const bytes expected {1, 3, 0x34, 0x12, 0, 0, 1, 0, 3, 0, 0, 0, 9, 8, 7, 0};
  EXPECT_TRUE(std::equal(expected.begin(), expected.end(), chunks[0].begin()));
}

TEST(AudioBitstreamProtocol, RejectsMalformedChunks) {
  auto good = chunks_of(pattern(3000, 1), 0, 1)[1];
  EXPECT_TRUE(parse_chunk(good));
  EXPECT_FALSE(parse_chunk(bytes(good.begin(), good.end() - 1)));  // Wrong size.
  auto bad = good;
  bad[0] = 2;  // Unknown version.
  EXPECT_FALSE(parse_chunk(bad));
  bad = good;
  bad[4] = 3;  // Index past the count.
  EXPECT_FALSE(parse_chunk(bad));
  bad = good;
  bad[6] = 9;  // Count doesn't match the length.
  EXPECT_FALSE(parse_chunk(bad));
  bad = good;
  bad[10] = 0x10;  // Length above the limit.
  EXPECT_FALSE(parse_chunk(bad));
}

TEST(AudioBitstreamProtocol, ReassemblesFramesAndDropsIncompleteOnes) {
  reassembler_t r;
  std::vector<received> out;
  auto emit = [&](std::span<const std::uint8_t> f, std::uint8_t flags) {
    out.push_back({bytes(f.begin(), f.end()), flags});
  };

  const auto a = pattern(4000, 1), b = pattern(5000, 2), c = pattern(100, 3);
  auto ca = chunks_of(a, audio_bitstream::burst_aligned | audio_bitstream::discontinuity, 1);
  auto cb = chunks_of(b, audio_bitstream::burst_aligned, 2);
  auto cc = chunks_of(c, audio_bitstream::burst_aligned, 3);

  for (const auto &chunk : ca) EXPECT_TRUE(r.push(chunk, emit));
  EXPECT_TRUE(r.push(ca.back(), emit));  // Duplicate of a delivered frame: ignored.
  for (std::size_t i = 0; i < cb.size(); ++i) {
    if (i != 2) EXPECT_TRUE(r.push(cb[i], emit));  // Chunk 2 lost.
  }
  for (const auto &chunk : cc) EXPECT_TRUE(r.push(chunk, emit));

  ASSERT_EQ(out.size(), 2u);
  EXPECT_EQ(out[0].data, a);
  EXPECT_EQ(out[0].flags, audio_bitstream::burst_aligned | audio_bitstream::discontinuity);
  EXPECT_EQ(out[1].data, c);
  EXPECT_EQ(out[1].flags, audio_bitstream::burst_aligned | audio_bitstream::discontinuity);
  EXPECT_EQ(r.lost_frames(), 1u);
}

TEST(AudioBitstreamProtocol, AFrameLostWhollyIsStillCounted) {
  // A silent MAT frame packs into one chunk; losing it leaves no partial frame
  // behind, only a skipped frame id.
  reassembler_t r;
  std::vector<received> out;
  auto emit = [&](std::span<const std::uint8_t> f, std::uint8_t flags) {
    out.push_back({bytes(f.begin(), f.end()), flags});
  };
  for (const auto &chunk : chunks_of(pattern(100, 1), 0, 10)) r.push(chunk, emit);
  // Frame 11 never arrives.
  for (const auto &chunk : chunks_of(pattern(100, 2), 0, 12)) r.push(chunk, emit);
  for (const auto &chunk : chunks_of(pattern(100, 3), 0, 13)) r.push(chunk, emit);

  ASSERT_EQ(out.size(), 3u);
  EXPECT_TRUE(out[1].flags & audio_bitstream::discontinuity);
  EXPECT_FALSE(out[2].flags & audio_bitstream::discontinuity);
  EXPECT_EQ(r.lost_frames(), 1u);
}

TEST(AudioBitstreamProtocol, FrameIdsWrap) {
  reassembler_t r;
  std::vector<received> out;
  auto emit = [&](std::span<const std::uint8_t> f, std::uint8_t flags) {
    out.push_back({bytes(f.begin(), f.end()), flags});
  };
  for (const std::uint16_t id : {std::uint16_t(0xFFFE), std::uint16_t(0xFFFF), std::uint16_t(0), std::uint16_t(1)}) {
    for (const auto &chunk : chunks_of(pattern(2000, id), 0, id)) r.push(chunk, emit);
  }
  EXPECT_EQ(out.size(), 4u);
  EXPECT_EQ(r.lost_frames(), 0u);
}

TEST(AudioBitstreamProtocol, InconsistentChunkOfTheCurrentFrameIsRejected) {
  reassembler_t r;
  int frames = 0;
  auto emit = [&](std::span<const std::uint8_t>, std::uint8_t) {
    ++frames;
  };
  const auto chunks = chunks_of(pattern(3000, 4), 0, 5);
  EXPECT_TRUE(r.push(chunks[0], emit));
  // Same frame id, different length: a different frame claiming the same id.
  auto other = chunks_of(pattern(5000, 5), 0, 5)[1];
  EXPECT_FALSE(r.push(other, emit));
  EXPECT_TRUE(r.push(chunks[1], emit));
  EXPECT_TRUE(r.push(chunks[2], emit));
  EXPECT_EQ(frames, 1);
}

// The whole host-to-client path on a real capture: framer, pack, chunk,
// reassemble, unpack. Set VIBEPOLLO_MAT_TAP_WAV to a tap_<n>.wav; skipped otherwise.
TEST(AudioBitstreamProtocol, PacingSpreadsEachFrameOverTheWindow) {
  using namespace std::chrono_literals;
  // YouTube-like frame: 5 chunks + 3 parity over 15 ms.
  EXPECT_EQ(packet_spacing(5), 1875us);
  // A game in Atmos: 13 chunks + 7 parity.
  EXPECT_EQ(packet_spacing(13), 750us);
  // One-chunk AC-3 frames and a huge frame hit the limits.
  EXPECT_EQ(packet_spacing(1), MAX_PACKET_SPACING);
  EXPECT_EQ(packet_spacing(60), MIN_PACKET_SPACING);
  // The rate the spacing allows always covers the frame's packets in its 20 ms.
  for (std::size_t chunks = 1; chunks <= 24; ++chunks) {
    const auto packets = chunks + (chunks + 1) / 2;
    EXPECT_LE(packet_spacing(chunks) * packets, 20ms) << chunks << " chunks";
  }
}

TEST(AudioBitstreamProtocol, RealCaptureSurvivesTheWholeWirePath) {
  const char *path = std::getenv("VIBEPOLLO_MAT_TAP_WAV");
  if (!path || !*path) {
    GTEST_SKIP() << "VIBEPOLLO_MAT_TAP_WAV is not set";
  }
  std::ifstream file(path, std::ios::binary);
  ASSERT_TRUE(file) << path;
  const bytes wav((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
  std::size_t at = 12, data_at = 0;
  while (at + 8 <= wav.size()) {
    const std::uint32_t length = wav[at + 4] | (wav[at + 5] << 8) | (wav[at + 6] << 16) | (std::uint32_t(wav[at + 7]) << 24);
    if (std::equal(wav.begin() + at, wav.begin() + at + 4, "data")) {
      data_at = at + 8;
      break;
    }
    at += 8 + length + (length & 1);
  }
  ASSERT_NE(data_at, 0u);
  const std::span<const std::uint8_t> raw(wav.data() + data_at, wav.size() - data_at);

  audio_bitstream::framer framer;
  reassembler_t reassembler;
  bytes rebuilt;
  std::uint16_t frame_id = 0;
  std::size_t chunks = 0, max_chunks = 0;
  bool ok = true;
  auto on_frame = [&](std::span<const std::uint8_t> f, std::uint8_t) {
    ok = ok && audio_bitstream::unpack(f, rebuilt);
  };
  auto on_raw = [&](std::span<const std::uint8_t> frame, std::uint8_t flags) {
    const auto packed = audio_bitstream::pack(frame, flags);
    ok = ok && packed.has_value();
    if (!packed) return;
    std::size_t n = 0;
    chunk_frame(*packed, flags, frame_id++, [&](std::span<const std::uint8_t, CHUNK_BYTES> c) {
      ++n;
      ok = reassembler.push(c, on_frame) && ok;
    });
    chunks += n;
    max_chunks = std::max(max_chunks, n);
  };
  for (std::size_t pos = 0; pos < raw.size(); pos += 30720) {
    framer.feed(raw.subspan(pos, std::min<std::size_t>(30720, raw.size() - pos)), on_raw);
  }
  framer.flush(on_raw);

  ASSERT_TRUE(ok);
  ASSERT_EQ(rebuilt.size(), raw.size());
  EXPECT_TRUE(std::equal(rebuilt.begin(), rebuilt.end(), raw.begin()));
  EXPECT_EQ(reassembler.lost_frames(), 0u);

  const double seconds = double(raw.size()) / 3072000.0;
  const double data_kbps = chunks * CHUNK_BYTES * 8.0 / seconds / 1000.0;
  std::printf("[ wire     ] %.0f packets/s (max %zu per frame), %.0f kbit/s payload, %.0f kbit/s with 4+2 FEC\n",
              chunks / seconds, max_chunks, data_kbps, data_kbps * 1.5);
  EXPECT_LT(data_kbps, BITRATE_BUDGET_KBPS);
}
