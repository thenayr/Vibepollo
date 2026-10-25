/**
 * @file tests/unit/test_atmos_tap.cpp
 * @brief Tests the Atmos probe tap reader against an in-memory tap that
 *        publishes bytes exactly as the driver does.
 */
#include "src/platform/windows/atmos_tap.h"

#include <cstdlib>
#include <fstream>
#include <gtest/gtest.h>
#include <random>

namespace {
  using namespace platf::audio::atmos_tap;
  using bytes = std::vector<std::uint8_t>;

  constexpr std::size_t header_page = 4096;

  // WAVEFORMATEXTENSIBLE for an IEC 61937 subtype {0000xxxx-0cea-0010-8000-00aa00389b71},
  // or a PCM-family one when `middle` is 0.
  bytes extensible(std::uint16_t channels, std::uint32_t rate, std::uint32_t code, std::uint16_t middle) {
    bytes f(40, 0);
    const auto put = [&](std::size_t at, std::uint32_t v, unsigned count) {
      for (unsigned i = 0; i < count; ++i) f[at + i] = std::uint8_t(v >> (i * 8));
    };
    put(0, 0xFFFE, 2);
    put(2, channels, 2);
    put(4, rate, 4);
    put(8, rate * channels * 2, 4);
    put(12, channels * 2, 2);
    put(14, 16, 2);
    put(16, 22, 2);
    put(18, 16, 2);
    put(24, code, 4);
    put(28, middle, 2);
    const std::uint8_t tail[] {0x10, 0x00, 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71};
    std::copy(std::begin(tail), std::end(tail), f.begin() + 30);
    return f;
  }

  const bytes mat21_format = extensible(8, 192000, 0x030c, 0x0cea);

  // The driver side of the tap (ProbeTapBeginStream/Write/EndStream).
  class fake_tap {
  public:
    explicit fake_tap(std::size_t ring_size):
        mem_(header_page + ring_size) {
      h()->magic = magic;
      h()->version = version;
      h()->header_size = header_page;
      h()->ring_size = std::uint32_t(ring_size);
    }

    view_t view() {
      return make_view(mem_.data(), mem_.size());
    }

    void begin_stream(const bytes &format) {
      std::fill(std::begin(h()->format), std::end(h()->format), 0);
      std::copy(format.begin(), format.end(), h()->format);
      h()->format_size = std::uint32_t(format.size());
      h()->stream_start_pos = h()->write_pos;
      h()->stream_active = 1;
      ++h()->stream_serial;
    }

    void write(std::span<const std::uint8_t> data) {
      const auto ring = h()->ring_size;
      for (std::size_t i = 0; i < data.size(); ++i) {
        mem_[header_page + std::size_t((h()->write_pos + std::int64_t(i)) % ring)] = data[i];
      }
      h()->write_pos += std::int64_t(data.size());
    }

    void end_stream() {
      h()->stream_active = 0;
    }

  private:
    header_t *h() {
      return reinterpret_cast<header_t *>(mem_.data());
    }

    bytes mem_;
  };

  struct recorded_events {
    std::vector<stream_info_t> started;
    std::vector<bytes> data;  // Per stream.
    std::vector<std::uint64_t> lost;
    int ended = 0;

    void stream_started(const stream_info_t &info) {
      started.push_back(info);
      data.emplace_back();
    }

    void data_chunk(std::span<const std::uint8_t> chunk) {
      data.back().insert(data.back().end(), chunk.begin(), chunk.end());
    }

    void lost_bytes(std::uint64_t n) {
      lost.push_back(n);
    }
  };

  // ring_reader_t's event names, forwarded to recorded_events.
  struct reader_events {
    recorded_events &r;

    void stream_started(const stream_info_t &info) {
      r.stream_started(info);
    }

    void data(std::span<const std::uint8_t> chunk) {
      r.data_chunk(chunk);
    }

    void lost(std::uint64_t n) {
      r.lost_bytes(n);
    }

    void stream_ended() {
      ++r.ended;
    }
  };

  struct frame_sink {
    std::vector<stream_info_t> started;
    std::vector<packed_frame_t> frames;
    int ended = 0;

    void stream_started(const stream_info_t &info) {
      started.push_back(info);
    }

    void frame(packed_frame_t &&f) {
      frames.push_back(std::move(f));
    }

    void stream_ended() {
      ++ended;
    }

    bytes rebuild() const {
      bytes out;
      for (const auto &f : frames) {
        EXPECT_TRUE(audio_bitstream::unpack(f.bytes, out));
      }
      return out;
    }
  };

  bytes pattern(std::size_t n, std::uint32_t seed) {
    bytes b(n);
    std::mt19937 rng(seed);
    for (auto &v : b) v = std::uint8_t(rng());
    return b;
  }

  bytes burst(std::size_t period, std::uint32_t seed) {
    bytes b(period, 0);
    std::copy(audio_bitstream::burst_sync.begin(), audio_bitstream::burst_sync.end(), b.begin());
    b[4] = 22;
    const auto data = pattern(4000, seed);
    std::copy(data.begin(), data.end(), b.begin() + 8);
    return b;
  }
}  // namespace

TEST(AtmosTap, ParsesStreamFormats) {
  const auto mat = parse_format(mat21_format);
  EXPECT_EQ(mat.encoding, encoding_e::mat21);
  EXPECT_TRUE(mat.bitstream());
  EXPECT_EQ(mat.channels, 8);
  EXPECT_EQ(mat.sample_rate, 192000u);
  EXPECT_EQ(mat.bits_per_sample, 16);

  EXPECT_EQ(parse_format(extensible(8, 192000, 0x010c, 0x0cea)).encoding, encoding_e::mat20);
  EXPECT_EQ(parse_format(extensible(8, 192000, 0x000c, 0x0cea)).encoding, encoding_e::mlp);
  EXPECT_EQ(parse_format(extensible(8, 192000, 0x010a, 0x0cea)).encoding, encoding_e::dolby_digital_plus_atmos);
  EXPECT_EQ(parse_format(extensible(8, 48000, 0x0001, 0)).encoding, encoding_e::pcm);
  EXPECT_FALSE(parse_format(extensible(8, 48000, 0x0001, 0)).bitstream());
  EXPECT_EQ(parse_format(extensible(2, 48000, 0x0003, 0)).encoding, encoding_e::ieee_float);

  bytes ac3(18, 0);
  ac3[0] = 0x92;
  ac3[2] = 2;
  EXPECT_EQ(parse_format(ac3).encoding, encoding_e::ac3);
  EXPECT_TRUE(parse_format(ac3).bitstream());

  EXPECT_EQ(parse_format(bytes(10, 0xFF)).encoding, encoding_e::unknown);
  auto foreign = mat21_format;
  foreign[39] ^= 1;  // Not a KSDATAFORMAT_SUBTYPE GUID.
  EXPECT_EQ(parse_format(foreign).encoding, encoding_e::unknown);
}

TEST(AtmosTap, RejectsAForeignLayout) {
  fake_tap tap(1 << 16);
  EXPECT_TRUE(tap.view().header);

  bytes mem(header_page + 1024, 0);
  auto *h = reinterpret_cast<header_t *>(mem.data());
  h->magic = magic;
  h->version = version;
  h->header_size = header_page;
  h->ring_size = 1 << 16;  // Larger than what's mapped.
  EXPECT_FALSE(make_view(mem.data(), mem.size()).header);
  h->ring_size = 1024;
  EXPECT_TRUE(make_view(mem.data(), mem.size()).header);
  h->version = version + 1;
  EXPECT_FALSE(make_view(mem.data(), mem.size()).header);
  h->version = version;
  h->magic = 0;
  EXPECT_FALSE(make_view(mem.data(), mem.size()).header);
  EXPECT_FALSE(make_view(nullptr, mem.size()).header);
}

TEST(AtmosTap, ReaderFollowsAStreamFromItsStartAcrossRingWraps) {
  fake_tap tap(1 << 16);
  recorded_events r;
  reader_events ev {r};
  ring_reader_t reader(tap.view(), 5000);
  reader.poll(ev);
  EXPECT_TRUE(r.started.empty());

  tap.begin_stream(mat21_format);
  const auto stream = pattern(300000, 1);
  for (std::size_t at = 0; at < stream.size(); at += 7777) {
    tap.write(std::span(stream).subspan(at, std::min<std::size_t>(7777, stream.size() - at)));
    reader.poll(ev);
  }
  tap.end_stream();
  reader.poll(ev);

  ASSERT_EQ(r.started.size(), 1u);
  EXPECT_TRUE(r.started[0].from_start);
  EXPECT_EQ(r.started[0].format.encoding, encoding_e::mat21);
  EXPECT_EQ(r.data[0], stream);
  EXPECT_TRUE(r.lost.empty());
  EXPECT_EQ(r.ended, 1);
}

TEST(AtmosTap, ReaderAttachingMidStreamStartsAtTheCurrentPosition) {
  fake_tap tap(1 << 16);
  tap.begin_stream(mat21_format);
  tap.write(pattern(1000, 2));

  recorded_events r;
  reader_events ev {r};
  ring_reader_t reader(tap.view());
  reader.poll(ev);
  const auto later = pattern(500, 3);
  tap.write(later);
  reader.poll(ev);

  ASSERT_EQ(r.started.size(), 1u);
  EXPECT_FALSE(r.started[0].from_start);
  EXPECT_EQ(r.data[0], later);
}

TEST(AtmosTap, ReaderReportsBytesTheWriterOverwrote) {
  constexpr std::size_t ring = 1 << 16;
  fake_tap tap(ring);
  recorded_events r;
  reader_events ev {r};
  ring_reader_t reader(tap.view());
  reader.poll(ev);

  tap.begin_stream(mat21_format);
  const auto stream = pattern(200000, 4);
  tap.write(stream);  // Laps the 64 KiB ring before the reader looks.
  reader.poll(ev);

  ASSERT_EQ(r.started.size(), 1u);
  EXPECT_FALSE(r.started[0].from_start);  // Its first bytes are gone.
  ASSERT_EQ(r.lost.size(), 1u);
  EXPECT_EQ(r.lost[0], stream.size() - ring / 2);
  // What it did read is the newest half of the ring, exactly.
  EXPECT_EQ(r.data[0], bytes(stream.end() - ring / 2, stream.end()));
}

TEST(AtmosTap, ReaderDrainsTheOldStreamBeforeStartingTheNextOne) {
  fake_tap tap(1 << 16);
  recorded_events r;
  reader_events ev {r};
  ring_reader_t reader(tap.view());
  reader.poll(ev);

  tap.begin_stream(mat21_format);
  reader.poll(ev);
  const auto first = pattern(3000, 5);
  tap.write(first);
  // Stream 1's last bytes and stream 2's start land between two polls.
  tap.begin_stream(extensible(8, 48000, 0x0001, 0));
  const auto second = pattern(2000, 6);
  tap.write(second);
  reader.poll(ev);

  ASSERT_EQ(r.started.size(), 2u);
  EXPECT_EQ(r.data[0], first);
  EXPECT_EQ(r.data[1], second);
  EXPECT_EQ(r.ended, 1);
  EXPECT_EQ(r.started[1].format.encoding, encoding_e::pcm);
  EXPECT_TRUE(r.started[1].from_start);
}

TEST(AtmosTap, SourcePacksBurstsAndFlushesWhenTheStreamEnds) {
  fake_tap tap(1 << 18);
  frame_sink sink;
  bitstream_source_t<frame_sink> source(tap.view(), sink);
  source.poll();

  tap.begin_stream(mat21_format);
  bytes stream;
  for (std::uint32_t i = 0; i < 6; ++i) {
    const auto b = burst(61440, i);
    stream.insert(stream.end(), b.begin(), b.end());
  }
  for (std::size_t at = 0; at < stream.size(); at += 10000) {
    tap.write(std::span(stream).subspan(at, std::min<std::size_t>(10000, stream.size() - at)));
    source.poll();
  }
  tap.end_stream();
  source.poll();

  ASSERT_EQ(sink.started.size(), 1u);
  EXPECT_EQ(sink.ended, 1);
  ASSERT_EQ(sink.frames.size(), 6u);
  for (std::size_t i = 0; i < sink.frames.size(); ++i) {
    EXPECT_EQ(sink.frames[i].raw_length, 61440u);
    EXPECT_TRUE(sink.frames[i].flags & audio_bitstream::burst_aligned);
    EXPECT_EQ(bool(sink.frames[i].flags & audio_bitstream::discontinuity), i == 0);
    EXPECT_EQ(sink.frames[i].stream_serial, 1);
    EXPECT_LT(sink.frames[i].bytes.size(), 4200u);
  }
  EXPECT_EQ(sink.rebuild(), stream);
}

TEST(AtmosTap, SourceMarksTheFrameAfterLostBytes) {
  constexpr std::size_t ring = 1 << 18;
  fake_tap tap(ring);
  frame_sink sink;
  bitstream_source_t<frame_sink> source(tap.view(), sink);
  source.poll();

  tap.begin_stream(mat21_format);
  for (std::uint32_t i = 0; i < 3; ++i) tap.write(burst(61440, i));
  source.poll();
  const auto before = sink.frames.size();
  for (std::uint32_t i = 3; i < 15; ++i) tap.write(burst(61440, i));  // Laps the ring.
  source.poll();

  EXPECT_GT(source.lost_bytes(), 0u);
  ASSERT_GT(sink.frames.size(), before + 1);
  // The frame buffered before the loss is flushed as-is, then the next is marked.
  EXPECT_FALSE(sink.frames[before].flags & audio_bitstream::discontinuity);
  EXPECT_TRUE(sink.frames[before + 1].flags & audio_bitstream::discontinuity);
}

// Feeds a real capture (tapreader's tap_<n>.wav) through an in-memory tap in
// uneven writes, as the driver would, and checks the packed frames rebuild it.
TEST(AtmosTap, RealCaptureThroughTheTapRoundTripsBitExact) {
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

  fake_tap tap(8 << 20);
  frame_sink sink;
  bitstream_source_t<frame_sink> source(tap.view(), sink);
  source.poll();
  tap.begin_stream(mat21_format);
  std::mt19937 rng(99);
  for (std::size_t pos = 0; pos < raw.size();) {
    const auto n = std::min<std::size_t>(1 + rng() % 120000, raw.size() - pos);
    tap.write(raw.subspan(pos, n));
    pos += n;
    if (rng() % 3 == 0) source.poll();  // The reader doesn't see every write.
  }
  tap.end_stream();
  source.poll();

  EXPECT_EQ(source.lost_bytes(), 0u);
  const auto rebuilt = sink.rebuild();
  ASSERT_EQ(rebuilt.size(), raw.size());
  EXPECT_TRUE(std::equal(rebuilt.begin(), rebuilt.end(), raw.begin()));
}
