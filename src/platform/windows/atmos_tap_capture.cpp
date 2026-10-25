/**
 * @file src/platform/windows/atmos_tap_capture.cpp
 * @brief Maps the Atmos probe tap and polls it on a high-resolution timer.
 */
#include "atmos_tap_capture.h"

#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <type_traits>

#include <windows.h>

namespace platf::audio::atmos_tap {
  namespace {
    // MAT bursts are 20 ms; polling every 2 ms keeps added latency well below one burst.
    constexpr LONGLONG poll_interval_100ns = 2 * 10000;

    struct handle_closer {
      void operator()(HANDLE h) const {
        if (h) CloseHandle(h);
      }
    };
    using unique_handle = std::unique_ptr<std::remove_pointer_t<HANDLE>, handle_closer>;

    struct view_unmapper {
      void operator()(const void *p) const {
        if (p) UnmapViewOfFile(p);
      }
    };
    using unique_view = std::unique_ptr<const void, view_unmapper>;

    std::string describe(DWORD error) {
      switch (error) {
        case ERROR_FILE_NOT_FOUND:
          return "the Atmos probe driver isn't loaded (no tap section)";
        case ERROR_ACCESS_DENIED:
          return "access denied: the tap is readable only by Administrators and SYSTEM";
        default:
          return "Windows error " + std::to_string(error);
      }
    }
  }  // namespace

  struct capture_t::impl_t {
    unique_handle mapping;
    unique_view base;
    view_t view;

    std::thread thread;
    std::atomic<bool> stopping {false};
    std::atomic<std::uint64_t> lost {0};

    struct sink_t {
      callbacks_t callbacks;

      void stream_started(const stream_info_t &info) {
        if (callbacks.stream_started) callbacks.stream_started(info);
      }

      void frame(packed_frame_t &&frame) {
        if (callbacks.frame) callbacks.frame(std::move(frame));
      }

      void stream_ended() {
        if (callbacks.stream_ended) callbacks.stream_ended();
      }
    };

    void run(callbacks_t callbacks) {
      SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
      unique_handle timer {CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS)};

      sink_t sink {std::move(callbacks)};
      bitstream_source_t<sink_t> source {view, sink};
      while (!stopping.load(std::memory_order_acquire)) {
        source.poll();
        lost.store(source.lost_bytes(), std::memory_order_relaxed);

        LARGE_INTEGER due;
        due.QuadPart = -poll_interval_100ns;
        if (timer && SetWaitableTimer(timer.get(), &due, 0, nullptr, nullptr, FALSE)) {
          WaitForSingleObject(timer.get(), INFINITE);
        } else {
          Sleep(1);
        }
      }
      source.poll();
      source.finish();
      lost.store(source.lost_bytes(), std::memory_order_relaxed);
    }
  };

  std::unique_ptr<capture_t> capture_t::open(std::string &error) {
    auto impl = std::make_unique<impl_t>();
    impl->mapping.reset(OpenFileMappingW(FILE_MAP_READ, FALSE, section_name));
    if (!impl->mapping) {
      error = describe(GetLastError());
      return nullptr;
    }

    impl->base.reset(MapViewOfFile(impl->mapping.get(), FILE_MAP_READ, 0, 0, 0));
    if (!impl->base) {
      error = describe(GetLastError());
      return nullptr;
    }

    MEMORY_BASIC_INFORMATION info {};
    if (!VirtualQuery(impl->base.get(), &info, sizeof(info))) {
      error = describe(GetLastError());
      return nullptr;
    }

    impl->view = make_view(impl->base.get(), info.RegionSize);
    if (!impl->view.header) {
      error = "the tap's header doesn't match this build (driver and Vibepollo out of sync)";
      return nullptr;
    }
    return std::unique_ptr<capture_t>(new capture_t(std::move(impl)));
  }

  capture_t::capture_t(std::unique_ptr<impl_t> impl):
      impl_(std::move(impl)) {}

  capture_t::~capture_t() {
    stop();
  }

  bool capture_t::start(callbacks_t callbacks) {
    if (impl_->thread.joinable()) return false;
    impl_->stopping.store(false, std::memory_order_release);
    impl_->thread = std::thread([impl = impl_.get(), callbacks = std::move(callbacks)]() mutable {
      impl->run(std::move(callbacks));
    });
    return true;
  }

  void capture_t::stop() {
    if (!impl_ || !impl_->thread.joinable()) return;
    impl_->stopping.store(true, std::memory_order_release);
    impl_->thread.join();
  }

  std::uint64_t capture_t::lost_bytes() const {
    return impl_->lost.load(std::memory_order_relaxed);
  }
}  // namespace platf::audio::atmos_tap
