/**
 * @file src/platform/windows/atmos_tap_capture.h
 * @brief Opens the Atmos probe driver's tap and reads it on a dedicated thread.
 */
#pragma once

#include "atmos_tap.h"

#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace platf::audio::atmos_tap {
  /** @brief Name of the render endpoint the probe driver registers. */
  inline constexpr std::string_view endpoint_name = "Vibepollo Atmos Probe";

  /**
   * @brief Whether the probe's render endpoint is active.
   *
   * The driver creates the tap only once a stream plays to the probe, so this
   * is how to tell the probe is installed before anything has played to it.
   */
  bool endpoint_present();

  /**
   * @brief Captures packed IEC 61937 frames from the probe tap until stopped.
   *
   * Callbacks run on the capture thread, in stream order, and must not block
   * for long: the ring holds about 2.7 s of MAT before data is lost.
   */
  class capture_t {
  public:
    struct callbacks_t {
      std::function<void(const stream_info_t &)> stream_started;
      std::function<void(packed_frame_t &&)> frame;
      std::function<void()> stream_ended;
    };

    /**
     * @brief Map the tap read-only.
     * @param error Set to a readable reason when the tap can't be opened.
     * @param not_created Optional; set to true when the tap doesn't exist yet,
     *        which is also the case while the driver is loaded but nothing has
     *        played to the probe. Worth retrying then.
     * @return The capture, not yet started, or nullptr.
     *
     * Needs Administrator or SYSTEM: the driver grants nothing else access.
     */
    static std::unique_ptr<capture_t> open(std::string &error, bool *not_created = nullptr);

    ~capture_t();
    capture_t(const capture_t &) = delete;
    capture_t &operator=(const capture_t &) = delete;

    /** @brief Start the capture thread. Returns false if it's already running. */
    bool start(callbacks_t callbacks);

    /** @brief Stop the thread and flush the current stream's buffered frame. */
    void stop();

    /** @brief Bytes the reader couldn't read before the driver overwrote them. */
    std::uint64_t lost_bytes() const;

  private:
    struct impl_t;
    explicit capture_t(std::unique_ptr<impl_t> impl);
    std::unique_ptr<impl_t> impl_;
  };
}  // namespace platf::audio::atmos_tap
