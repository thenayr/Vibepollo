/**
 * @file src/platform/windows/atmos_tap.h
 * @brief Reads the Vibepollo Atmos probe driver's shared-memory tap and turns
 *        it into packed IEC 61937 frames.
 *
 * The probe driver copies every byte Windows renders to its endpoint, unmodified,
 * into a named shared ring (Global\VibepolloAtmosProbeTap). With Dolby Atmos for
 * home theater active on that endpoint, the bytes are a Dolby MAT bitstream.
 * This header is the platform-neutral part: the shared layout, a reader that
 * follows the driver's publication protocol, and a source that frames and packs
 * what it reads. Opening the real section lives in atmos_tap_capture.cpp.
 */
#pragma once

#include "src/audio_bitstream.h"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <vector>

namespace platf::audio::atmos_tap {
  inline constexpr wchar_t section_name[] = L"Global\\VibepolloAtmosProbeTap";
  inline constexpr std::uint32_t magic = 0x50415456;  // 'VTAP'
  inline constexpr std::uint32_t version = 1;
  inline constexpr std::size_t format_max = 64;

  /**
   * @brief The tap's header page, matching PROBE_TAP_HEADER in the driver's
   *        probetap_shared.h. The ring follows at header_size bytes.
   *
   * The driver publishes bytes by copying them into the ring and then adding
   * their count to write_pos. A new stream sets format and stream_start_pos,
   * then increments stream_serial last.
   */
  struct header_t {
    std::uint32_t magic;
    std::uint32_t version;
    std::uint32_t header_size;
    std::uint32_t ring_size;
    std::int64_t write_pos;
    std::int64_t stream_start_pos;
    std::int32_t stream_serial;
    std::int32_t stream_active;
    std::uint32_t format_size;
    std::uint32_t reserved;
    std::uint8_t format[format_max];
    std::int64_t dropped_bytes;
  };

  static_assert(offsetof(header_t, write_pos) == 16);
  static_assert(offsetof(header_t, stream_serial) == 32);
  static_assert(offsetof(header_t, format) == 48);
  static_assert(offsetof(header_t, dropped_bytes) == 112);
  static_assert(sizeof(header_t) == 120);

  template<class T>
  T load_acquire(const volatile T &field) {
    const T value = field;
    std::atomic_thread_fence(std::memory_order_acquire);
    return value;
  }

  /** @brief A mapped tap: its header page and the ring after it. */
  struct view_t {
    const volatile header_t *header = nullptr;
    const std::uint8_t *ring = nullptr;
    std::size_t ring_size = 0;
  };

  /**
   * @brief Validate a mapped section of `mapped_size` bytes starting at `base`.
   * @return A usable view, or an empty one if the layout isn't what this build expects.
   */
  inline view_t make_view(const void *base, std::size_t mapped_size) {
    if (!base || mapped_size < sizeof(header_t)) return {};
    const auto *header = static_cast<const volatile header_t *>(base);
    if (load_acquire(header->magic) != magic || header->version != version) return {};
    const std::size_t header_size = header->header_size;
    const std::size_t ring_size = header->ring_size;
    if (header_size < sizeof(header_t) || ring_size == 0 || mapped_size < header_size || mapped_size - header_size < ring_size) {
      return {};
    }
    return {header, static_cast<const std::uint8_t *>(base) + header_size, ring_size};
  }

  enum class encoding_e {
    unknown,
    pcm,
    ieee_float,
    ac3,
    dolby_digital_plus,
    dolby_digital_plus_atmos,
    mlp,
    mat20,
    mat21,
    iec61937_other,
  };

  struct stream_format_t {
    std::uint16_t format_tag = 0;
    std::uint16_t channels = 0;
    std::uint32_t sample_rate = 0;
    std::uint16_t bits_per_sample = 0;
    encoding_e encoding = encoding_e::unknown;

    /** @brief True when the stream is an IEC 61937 bitstream rather than PCM. */
    bool bitstream() const {
      return encoding != encoding_e::unknown && encoding != encoding_e::pcm && encoding != encoding_e::ieee_float;
    }
  };

  /** @brief Decode the WAVEFORMATEX(TENSIBLE) bytes the driver recorded for a stream. */
  inline stream_format_t parse_format(std::span<const std::uint8_t> bytes) {
    stream_format_t f;
    const auto get = [&](std::size_t at, unsigned count) {
      std::uint32_t v = 0;
      for (unsigned i = 0; i < count; ++i) v |= std::uint32_t(bytes[at + i]) << (i * 8);
      return v;
    };
    if (bytes.size() < 16) return f;
    f.format_tag = std::uint16_t(get(0, 2));
    f.channels = std::uint16_t(get(2, 2));
    f.sample_rate = get(4, 4);
    f.bits_per_sample = std::uint16_t(get(14, 2));

    std::uint32_t code = f.format_tag;
    bool iec_family = false;
    if (f.format_tag == 0xFFFE) {  // WAVE_FORMAT_EXTENSIBLE: the subformat GUID decides.
      if (bytes.size() < 40) return f;
      // KSDATAFORMAT_SUBTYPE_* GUIDs share the tail 8000-00aa00389b71. PCM-style
      // subtypes use 0000-0010 in the middle, IEC 61937 ones use 0cea-0010.
      static constexpr std::uint8_t tail[] {0x10, 0x00, 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71};
      if (!std::equal(std::begin(tail), std::end(tail), bytes.begin() + 30)) return f;
      code = get(24, 4);
      const auto middle = get(28, 2);
      if (middle == 0x0cea) {
        iec_family = true;
      } else if (middle != 0) {
        return f;
      }
    }

    if (!iec_family) {
      switch (code) {
        case 0x0001: f.encoding = encoding_e::pcm; break;
        case 0x0003: f.encoding = encoding_e::ieee_float; break;
        case 0x0092: f.encoding = encoding_e::ac3; break;  // WAVE_FORMAT_DOLBY_AC3_SPDIF
        default: break;
      }
      return f;
    }
    switch (code) {
      case 0x000a: f.encoding = encoding_e::dolby_digital_plus; break;
      case 0x010a: f.encoding = encoding_e::dolby_digital_plus_atmos; break;
      case 0x000c: f.encoding = encoding_e::mlp; break;
      case 0x010c: f.encoding = encoding_e::mat20; break;
      case 0x030c:
      case 0x070c: f.encoding = encoding_e::mat21; break;
      default: f.encoding = encoding_e::iec61937_other; break;
    }
    return f;
  }

  struct stream_info_t {
    std::int32_t serial = 0;
    stream_format_t format;
    bool from_start = false;  ///< False when attaching to a stream that was already playing.
  };

  /**
   * @brief Follows the driver's publication protocol and reports stream events.
   *
   * Events must provide stream_started(const stream_info_t &), data(std::span<const std::uint8_t>),
   * lost(std::uint64_t bytes) and stream_ended(). poll() never blocks; call it
   * every few milliseconds. The ring holds about 2.7 s of MAT, so a reader only
   * loses data if it stalls for that long.
   */
  class ring_reader_t {
  public:
    explicit ring_reader_t(view_t view, std::size_t chunk_size = 1 << 16):
        view_(view),
        scratch_(chunk_size) {}

    bool in_stream() const {
      return in_stream_;
    }

    template<class Events>
    void poll(Events &events) {
      const auto *h = view_.header;
      if (!attached_) {
        attached_ = true;
        serial_ = load_acquire(h->stream_serial);
        if (load_acquire(h->stream_active)) start(serial_, false, events);
      }

      const auto serial = load_acquire(h->stream_serial);
      if (serial != serial_) {
        if (in_stream_) {
          // The old stream's last bytes sit just before the new stream's start.
          read_until(load_acquire(h->stream_start_pos), events);
          end(events);
        }
        start(serial, true, events);
      }

      if (!in_stream_) return;
      read_until(load_acquire(h->write_pos), events);
      if (!load_acquire(h->stream_active) && load_acquire(h->stream_serial) == serial_ &&
          load_acquire(h->write_pos) == pos_) {
        end(events);
      }
    }

  private:
    template<class Events>
    void start(std::int32_t serial, bool from_start, Events &events) {
      const auto *h = view_.header;
      serial_ = serial;
      stream_info_t info;
      info.serial = serial;
      std::uint8_t format[format_max];
      for (std::size_t i = 0; i < format_max; ++i) format[i] = h->format[i];
      const std::size_t format_size = std::min<std::size_t>(h->format_size, format_max);
      info.format = parse_format(std::span<const std::uint8_t>(format, format_size));

      const auto write_pos = load_acquire(h->write_pos);
      pos_ = from_start ? load_acquire(h->stream_start_pos) : write_pos;
      if (pos_ > write_pos) {
        pos_ = write_pos;
        from_start = false;
      } else if (write_pos - pos_ > std::int64_t(view_.ring_size)) {
        from_start = false;  // Its start was already overwritten; read_until reports the loss.
      }
      info.from_start = from_start;
      in_stream_ = true;
      events.stream_started(info);
    }

    template<class Events>
    void end(Events &events) {
      in_stream_ = false;
      events.stream_ended();
    }

    template<class Events>
    void read_until(std::int64_t target, Events &events) {
      const auto ring = std::int64_t(view_.ring_size);
      if (target <= pos_) return;
      if (target - pos_ > ring) {
        // Keep the newest half of the ring; the rest is gone or about to be.
        const auto skip = target - pos_ - ring / 2;
        events.lost(std::uint64_t(skip));
        pos_ += skip;
      }
      while (pos_ < target) {
        const auto n = std::size_t(std::min<std::int64_t>(target - pos_, std::int64_t(scratch_.size())));
        const auto offset = std::size_t(pos_ % ring);
        const auto first = std::min(n, view_.ring_size - offset);
        std::memcpy(scratch_.data(), view_.ring + offset, first);
        std::memcpy(scratch_.data() + first, view_.ring, n - first);
        std::atomic_thread_fence(std::memory_order_acquire);
        // The copy is only valid if the writer didn't lap us while it ran.
        if (load_acquire(view_.header->write_pos) - pos_ > ring) {
          events.lost(n);
          pos_ += std::int64_t(n);
          continue;
        }
        events.data(std::span<const std::uint8_t>(scratch_.data(), n));
        pos_ += std::int64_t(n);
      }
    }

    view_t view_;
    std::vector<std::uint8_t> scratch_;
    bool attached_ = false;
    bool in_stream_ = false;
    std::int32_t serial_ = 0;
    std::int64_t pos_ = 0;
  };

  /** @brief One packed frame (audio_bitstream wire format) ready to send. */
  struct packed_frame_t {
    std::int32_t stream_serial = 0;
    std::uint8_t flags = 0;
    std::uint32_t raw_length = 0;
    std::vector<std::uint8_t> bytes;
  };

  /**
   * @brief Reads the tap, cuts the stream at IEC 61937 bursts and packs each frame.
   *
   * Sink must provide stream_started(const stream_info_t &), frame(packed_frame_t &&)
   * and stream_ended(). The first frame of a stream, and the first after lost
   * bytes, carry audio_bitstream::discontinuity.
   */
  template<class Sink>
  class bitstream_source_t {
  public:
    bitstream_source_t(view_t view, Sink &sink):
        reader_(view),
        events_ {*this},
        sink_(sink) {}

    void poll() {
      reader_.poll(events_);
    }

    /** @brief End the current stream early, e.g. when capture stops: flushes the buffered frame. */
    void finish() {
      if (reader_.in_stream()) events_.stream_ended();
    }

    std::uint64_t lost_bytes() const {
      return lost_bytes_;
    }

  private:
    struct events_t {
      bitstream_source_t &self;

      void stream_started(const stream_info_t &info) {
        self.framer_.reset();
        self.serial_ = info.serial;
        self.discontinuity_ = true;
        self.sink_.stream_started(info);
      }

      void data(std::span<const std::uint8_t> bytes) {
        self.framer_.feed(bytes, [this](std::span<const std::uint8_t> raw, std::uint8_t flags) {
          self.emit(raw, flags);
        });
      }

      void lost(std::uint64_t bytes) {
        // What's buffered is still valid; it just doesn't continue into what follows.
        self.framer_.flush([this](std::span<const std::uint8_t> raw, std::uint8_t flags) {
          self.emit(raw, flags);
        });
        self.lost_bytes_ += bytes;
        self.discontinuity_ = true;
      }

      void stream_ended() {
        self.framer_.flush([this](std::span<const std::uint8_t> raw, std::uint8_t flags) {
          self.emit(raw, flags);
        });
        self.sink_.stream_ended();
      }
    };

    void emit(std::span<const std::uint8_t> raw, std::uint8_t flags) {
      if (discontinuity_) {
        flags |= audio_bitstream::discontinuity;
        discontinuity_ = false;
      }
      auto packed = audio_bitstream::pack(raw, flags);
      if (!packed) return;  // Only frames above max_frame_bytes, which the framer never makes.
      packed_frame_t frame;
      frame.stream_serial = serial_;
      frame.flags = flags;
      frame.raw_length = std::uint32_t(raw.size());
      frame.bytes = std::move(*packed);
      sink_.frame(std::move(frame));
    }

    ring_reader_t reader_;
    events_t events_;
    Sink &sink_;
    audio_bitstream::framer framer_;
    std::int32_t serial_ = 0;
    bool discontinuity_ = true;
    std::uint64_t lost_bytes_ = 0;
  };
}  // namespace platf::audio::atmos_tap
