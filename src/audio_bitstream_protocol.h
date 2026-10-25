/**
 * @file src/audio_bitstream_protocol.h
 * @brief Wire contract for audio bitstream passthrough (Dolby MAT / IEC 61937).
 *
 * The contract is docs/audio-bitstream-protocol.md, shared with the client
 * forks (moonlight-common-c, Moonlight). A passthrough session carries packed
 * IEC 61937 frames (audio_bitstream.h) instead of Opus, cut into fixed-size
 * chunks so the existing audio RTP path, its encryption and its 4+2
 * Reed-Solomon FEC work unchanged.
 */
#pragma once

#include "audio_bitstream.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace audio_bitstream::protocol {
  /// Packed IEC 61937 frames, audio_bitstream wire version 1, in CHUNK_BYTES chunks.
  constexpr std::uint32_t FORMAT_IEC61937 = 0x1;

  /// RTSP DESCRIBE attribute: the formats this host can capture right now.
  constexpr std::string_view DESCRIBE_FORMATS = "a=x-ss-audio.bitstreamFormats:";
  /// RTSP ANNOUNCE attribute: the formats the client can play. The session uses
  /// a format only if both sides list it.
  constexpr std::string_view ANNOUNCE_FORMATS = "x-ss-audio.bitstreamFormats";

  /// RTP payload type of bitstream packets. FEC packets keep type 127 and put this
  /// in their FEC header. Opus stays 97, so a receiver can tell them apart.
  constexpr std::uint8_t RTP_PAYLOAD_TYPE = 100;

  /// Every bitstream RTP payload is exactly this many bytes before encryption.
  /// Audio FEC needs equal-size shards, and 12 RTP + 1216 AES-CBC bytes stays
  /// under the client's 1400-byte audio datagram limit.
  constexpr std::size_t CHUNK_BYTES = 1200;
  constexpr std::size_t CHUNK_HEADER_BYTES = 12;
  constexpr std::size_t CHUNK_DATA_BYTES = CHUNK_BYTES - CHUNK_HEADER_BYTES;
  constexpr std::uint8_t CHUNK_VERSION = 1;

  /// Largest packed frame a receiver must accept.
  constexpr std::uint32_t MAX_FRAME_BYTES = 1u << 19;

  /// Video bitrate the host sets aside for passthrough audio payload (FEC parity
  /// adds half again on the wire). Measured MAT packs to about 2.0 Mbit/s for
  /// sparse audio (YouTube, ~5 chunks per 20 ms frame) and 5.4 Mbit/s for a game
  /// in Atmos (~12 chunks per frame, ~900 packets per second with parity).
  constexpr int BITRATE_BUDGET_KBPS = 6000;

  /// Least time between two passthrough packets (data or FEC parity) on the wire.
  /// A frame's chunks and the block's parity would otherwise leave back to back,
  /// and a burst that collides with video loses its tail together with the parity
  /// meant to repair it. The cap this sets, 2,500 packets per second, is enough
  /// for ~16 Mbit/s of packed MAT; a game frame spreads over about 7 ms of its 20.
  /// (1.5 ms capped the sender below what a game needs and overflowed its queue.)
  constexpr std::chrono::microseconds PACKET_SPACING {400};

  /// How far the paced send schedule may run behind before it is reset. The
  /// sender catches up on that much (back to back) after a late wake-up.
  constexpr std::chrono::milliseconds PACING_MAX_BACKLOG {40};

  /**
   * Chunk header, little-endian:
   *   u8 version, u8 frame flags (audio_bitstream::frame_flag),
   *   u16 frame id, u16 chunk index, u16 chunk count, u32 packed frame length.
   * Then CHUNK_DATA_BYTES of the packed frame; the last chunk is zero-padded.
   */
  struct chunk_header_t {
    std::uint8_t flags = 0;
    std::uint16_t frame_id = 0;
    std::uint16_t index = 0;
    std::uint16_t count = 0;
    std::uint32_t frame_length = 0;
  };

  inline std::size_t chunks_for(std::size_t frame_length) {
    return frame_length ? (frame_length + CHUNK_DATA_BYTES - 1) / CHUNK_DATA_BYTES : 1;
  }

  /**
   * @brief Cut one packed frame into chunks.
   * @param emit Called with each CHUNK_BYTES chunk, in order.
   * @return False if the frame is too large to send.
   */
  template<class Emit>
  bool chunk_frame(std::span<const std::uint8_t> packed, std::uint8_t flags, std::uint16_t frame_id, Emit &&emit) {
    if (packed.size() > MAX_FRAME_BYTES) return false;
    const auto count = chunks_for(packed.size());
    std::array<std::uint8_t, CHUNK_BYTES> chunk;
    for (std::size_t index = 0; index < count; ++index) {
      chunk.fill(0);
      const auto put = [&](std::size_t at, std::uint32_t v, unsigned bytes) {
        for (unsigned i = 0; i < bytes; ++i) chunk[at + i] = std::uint8_t(v >> (i * 8));
      };
      put(0, CHUNK_VERSION, 1);
      put(1, flags, 1);
      put(2, frame_id, 2);
      put(4, std::uint32_t(index), 2);
      put(6, std::uint32_t(count), 2);
      put(8, std::uint32_t(packed.size()), 4);
      const auto offset = index * CHUNK_DATA_BYTES;
      const auto length = std::min(CHUNK_DATA_BYTES, packed.size() - offset);
      if (length) std::memcpy(chunk.data() + CHUNK_HEADER_BYTES, packed.data() + offset, length);
      emit(std::span<const std::uint8_t, CHUNK_BYTES>(chunk));
    }
    return true;
  }

  /** @brief Parse and validate a chunk header; network input. */
  inline std::optional<chunk_header_t> parse_chunk(std::span<const std::uint8_t> chunk) {
    if (chunk.size() != CHUNK_BYTES || chunk[0] != CHUNK_VERSION) return std::nullopt;
    const auto get = [&](std::size_t at, unsigned bytes) {
      std::uint32_t v = 0;
      for (unsigned i = 0; i < bytes; ++i) v |= std::uint32_t(chunk[at + i]) << (i * 8);
      return v;
    };
    chunk_header_t h;
    h.flags = chunk[1];
    h.frame_id = std::uint16_t(get(2, 2));
    h.index = std::uint16_t(get(4, 2));
    h.count = std::uint16_t(get(6, 2));
    h.frame_length = get(8, 4);
    if (h.frame_length > MAX_FRAME_BYTES || h.count != chunks_for(h.frame_length) || h.index >= h.count) {
      return std::nullopt;
    }
    return h;
  }

  /**
   * @brief Rebuilds packed frames from chunks, as a receiver does.
   *
   * Chunks arrive in RTP order. A frame missing any chunk is dropped, and the
   * next complete frame is flagged audio_bitstream::discontinuity so the
   * receiver resyncs its output.
   */
  class reassembler_t {
  public:
    /** @return False if the chunk is malformed (and ignored). */
    template<class Emit>
    bool push(std::span<const std::uint8_t> chunk, Emit &&emit) {
      const auto header = parse_chunk(chunk);
      if (!header) return false;

      if (!active_ || header->frame_id != frame_id_) {
        if (active_) {
          // The previous frame is lost if it never completed, and so is every
          // frame whose id was skipped entirely (e.g. a one-chunk frame).
          const auto skipped = std::uint16_t(header->frame_id - frame_id_ - 1);
          // An id older than the last frame is out of order: don't splice it in.
          const std::uint64_t lost = (done_ ? 0 : 1) + (skipped < 0x8000 ? skipped : 1);
          if (lost) {
            lost_frames_ += lost;
            pending_discontinuity_ = true;
          }
        }
        active_ = true;
        done_ = false;
        frame_id_ = header->frame_id;
        count_ = header->count;
        frame_length_ = header->frame_length;
        received_ = 0;
        have_.assign(count_, false);
        frame_.assign(std::size_t(count_) * CHUNK_DATA_BYTES, 0);
      } else if (header->count != count_ || header->frame_length != frame_length_) {
        return false;  // Disagrees with the frame's other chunks.
      } else if (done_) {
        return true;  // Duplicate of a chunk of a frame already delivered.
      }

      if (have_[header->index]) return true;
      have_[header->index] = true;
      std::memcpy(frame_.data() + std::size_t(header->index) * CHUNK_DATA_BYTES, chunk.data() + CHUNK_HEADER_BYTES, CHUNK_DATA_BYTES);
      if (++received_ == count_) {
        done_ = true;
        std::uint8_t flags = header->flags;
        if (pending_discontinuity_) {
          flags |= audio_bitstream::discontinuity;
          pending_discontinuity_ = false;
        }
        emit(std::span<const std::uint8_t>(frame_.data(), frame_length_), flags);
      }
      return true;
    }

    std::uint64_t lost_frames() const {
      return lost_frames_;
    }

  private:
    bool active_ = false;
    bool done_ = false;
    bool pending_discontinuity_ = false;
    std::uint16_t frame_id_ = 0;
    std::uint16_t count_ = 0;
    std::uint32_t frame_length_ = 0;
    std::uint16_t received_ = 0;
    std::vector<bool> have_;
    std::vector<std::uint8_t> frame_;
    std::uint64_t lost_frames_ = 0;
  };
}  // namespace audio_bitstream::protocol
