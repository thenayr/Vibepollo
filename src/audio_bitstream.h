// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <vector>

// Lossless transport framing for IEC 61937 bitstreams (Dolby MAT from Windows'
// "Dolby Atmos for home theater" encoder). The raw stream is 8 ch / 192 kHz /
// 16-bit, about 3 MB/s, but each 20 ms MAT burst carries only ~4.5 KB of data;
// the rest is zero stuffing. The framer cuts the stream at burst preambles, the
// packer stores each frame as zero-skip + data segments, and the unpacker
// rebuilds the exact bytes.
//
// Packed frame, little-endian:
//   u8 version, u8 flags, u16 segment count, u32 raw length,
//   then per segment: u16 zero bytes to skip, u16 data length, data bytes.
// Bytes after the last segment, up to the raw length, are zero.
namespace audio_bitstream {
  // IEC 61937 Pa/Pb sync words (0xF872, 0x4E1F) as little-endian 16-bit samples.
  constexpr std::array<std::uint8_t, 4> burst_sync {0x72, 0xF8, 0x1F, 0x4E};

  constexpr std::uint8_t wire_version = 1;
  constexpr std::size_t header_size = 8;
  constexpr std::size_t segment_header_size = 4;
  constexpr std::uint32_t max_frame_bytes = 1u << 18;
  // Without burst sync (lead-in, or a stream that isn't IEC 61937) the framer
  // still emits bounded frames: 20 ms of the 8 ch / 192 kHz container.
  constexpr std::uint32_t unaligned_frame_bytes = 61440;

  enum frame_flag : std::uint8_t {
    burst_aligned = 0x01,  // The frame starts at an IEC 61937 preamble.
    discontinuity = 0x02,  // Not contiguous with the previous frame: a new stream, or bytes were lost.
  };

  struct frame_header {
    std::uint8_t flags = 0;
    std::uint16_t segments = 0;
    std::uint32_t raw_length = 0;
  };

  inline bool is_burst_sync(std::span<const std::uint8_t> bytes, std::size_t at) {
    return at + burst_sync.size() <= bytes.size() &&
           std::memcmp(bytes.data() + at, burst_sync.data(), burst_sync.size()) == 0;
  }

  namespace detail {
    inline void put16(std::vector<std::uint8_t> &out, std::uint16_t v) {
      out.push_back(std::uint8_t(v));
      out.push_back(std::uint8_t(v >> 8));
    }

    inline std::uint32_t get(std::span<const std::uint8_t> bytes, std::size_t at, unsigned count) {
      std::uint32_t v = 0;
      for (unsigned i = 0; i < count; ++i) v |= std::uint32_t(bytes[at + i]) << (i * 8);
      return v;
    }
  }  // namespace detail

  inline std::optional<frame_header> parse_header(std::span<const std::uint8_t> packed) {
    if (packed.size() < header_size || packed[0] != wire_version) return std::nullopt;
    frame_header header;
    header.flags = packed[1];
    header.segments = std::uint16_t(detail::get(packed, 2, 2));
    header.raw_length = detail::get(packed, 4, 4);
    if (header.raw_length > max_frame_bytes) return std::nullopt;
    return header;
  }

  // Data runs absorb zero gaps shorter than a segment header, since splitting
  // there would cost more than storing the zeros.
  inline std::optional<std::vector<std::uint8_t>> pack(std::span<const std::uint8_t> raw, std::uint8_t flags) {
    if (raw.size() > max_frame_bytes) return std::nullopt;
    std::vector<std::uint8_t> out(header_size);
    std::size_t segments = 0;
    std::size_t emitted_to = 0;
    std::size_t i = 0;
    const std::size_t n = raw.size();
    while (true) {
      while (i < n && raw[i] == 0) ++i;
      if (i == n) break;
      const std::size_t start = i;
      std::size_t end = i;
      while (end < n && end - start < 0xFFFF) {
        if (raw[end] != 0) {
          ++end;
          continue;
        }
        std::size_t gap = end;
        while (gap < n && raw[gap] == 0 && gap - end < segment_header_size) ++gap;
        if (gap == n || gap - end >= segment_header_size || gap - start >= 0xFFFF) break;
        end = gap;
      }
      std::size_t skip = start - emitted_to;
      for (; skip > 0xFFFF; skip -= 0xFFFF, ++segments) {
        detail::put16(out, 0xFFFF);
        detail::put16(out, 0);
      }
      detail::put16(out, std::uint16_t(skip));
      detail::put16(out, std::uint16_t(end - start));
      out.insert(out.end(), raw.begin() + start, raw.begin() + end);
      ++segments;
      emitted_to = i = end;
    }
    if (segments > 0xFFFF) return std::nullopt;
    out[0] = wire_version;
    out[1] = flags;
    out[2] = std::uint8_t(segments);
    out[3] = std::uint8_t(segments >> 8);
    for (unsigned b = 0; b < 4; ++b) out[4 + b] = std::uint8_t(raw.size() >> (b * 8));
    return out;
  }

  // Appends the frame's raw bytes to `out`. Packed frames arrive from the
  // network, so any inconsistency rejects the whole frame and leaves `out` as
  // it was.
  inline bool unpack(std::span<const std::uint8_t> packed, std::vector<std::uint8_t> &out) {
    const auto header = parse_header(packed);
    if (!header) return false;
    const std::size_t base = out.size();
    out.resize(base + header->raw_length, 0);
    std::size_t at = header_size;
    std::size_t cursor = 0;
    for (unsigned s = 0; s < header->segments; ++s) {
      if (packed.size() - at < segment_header_size) {
        out.resize(base);
        return false;
      }
      const std::size_t skip = detail::get(packed, at, 2);
      const std::size_t length = detail::get(packed, at + 2, 2);
      at += segment_header_size;
      cursor += skip;
      if (cursor > header->raw_length || header->raw_length - cursor < length || packed.size() - at < length) {
        out.resize(base);
        return false;
      }
      std::memcpy(out.data() + base + cursor, packed.data() + at, length);
      at += length;
      cursor += length;
    }
    if (at != packed.size()) {
      out.resize(base);
      return false;
    }
    return true;
  }

  // Splits a raw byte stream, delivered in chunks of any size, into frames that
  // each start at a burst preamble. Once two preambles are seen, their distance
  // is the burst period for that burst type, and later bursts of the type are
  // cut only at that period, so a sync-like pattern inside burst data can't
  // split a frame. A different burst type or a missing preamble falls back to
  // searching. Preambles sit on 16-bit sample boundaries, and frames always end
  // on one, so every frame starts on a sample boundary too.
  class framer {
  public:
    template<class Emit>
    void feed(std::span<const std::uint8_t> bytes, Emit &&emit) {
      buffer_.insert(buffer_.end(), bytes.begin(), bytes.end());
      while (step(emit)) {}
    }

    // Emits whatever is buffered, e.g. when the stream closes.
    template<class Emit>
    void flush(Emit &&emit) {
      if (!buffer_.empty()) {
        emit(std::span<const std::uint8_t>(buffer_), aligned_ ? std::uint8_t(burst_aligned) : std::uint8_t(0));
      }
      reset();
    }

    void reset() {
      buffer_.clear();
      aligned_ = false;
      period_ = 0;
      period_type_ = 0;
      scan_from_ = 0;
    }

    std::size_t period() const { return period_; }

  private:
    template<class Emit>
    void cut(std::size_t length, Emit &emit) {
      emit(std::span<const std::uint8_t>(buffer_.data(), length), aligned_ ? std::uint8_t(burst_aligned) : std::uint8_t(0));
      buffer_.erase(buffer_.begin(), buffer_.begin() + std::ptrdiff_t(length));
      scan_from_ = 0;
    }

    // Next preamble at an even offset >= from, or 0 if none is complete yet.
    std::size_t find_sync(std::size_t from) {
      std::size_t at = from + (from & 1);
      for (; at + burst_sync.size() <= buffer_.size(); at += 2) {
        if (is_burst_sync(buffer_, at)) return at;
      }
      scan_from_ = at;
      return 0;
    }

    // Pc's low 7 bits: the burst's data type (22 = MAT, 1 = AC-3, ...).
    std::uint8_t burst_type() const {
      return buffer_.size() > 4 ? std::uint8_t(buffer_[4] & 0x7F) : std::uint8_t(0);
    }

    template<class Emit>
    bool step(Emit &emit) {
      if (aligned_ && period_) {
        if (buffer_.size() <= 4) return false;
        if (burst_type() != period_type_) {
          period_ = 0;  // Another burst type has its own period.
        } else {
          if (buffer_.size() < period_ + burst_sync.size()) return false;
          if (is_burst_sync(buffer_, period_)) {
            cut(period_, emit);
            return true;
          }
          period_ = 0;  // Sync lost; find the next preamble the slow way.
        }
      }
      if (aligned_) {
        if (const auto next = find_sync(std::max<std::size_t>(scan_from_, 2))) {
          period_ = next;
          period_type_ = burst_type();
          cut(next, emit);
          return true;
        }
        if (buffer_.size() >= max_frame_bytes + burst_sync.size()) {
          cut(max_frame_bytes, emit);
          aligned_ = false;
          return true;
        }
        return false;
      }
      if (is_burst_sync(buffer_, 0)) {
        aligned_ = true;
        return true;
      }
      if (const auto next = find_sync(std::max<std::size_t>(scan_from_, 2))) {
        cut(next, emit);
        aligned_ = true;
        return true;
      }
      if (buffer_.size() >= unaligned_frame_bytes + burst_sync.size()) {
        cut(unaligned_frame_bytes, emit);
        return true;
      }
      return false;
    }

    std::vector<std::uint8_t> buffer_;
    bool aligned_ = false;
    std::size_t period_ = 0;
    std::uint8_t period_type_ = 0;
    std::size_t scan_from_ = 0;
  };
}  // namespace audio_bitstream
