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
#include <mmdeviceapi.h>
#include <propidl.h>

namespace platf::audio::atmos_tap {
  namespace {
    // MAT bursts are 20 ms; polling every 2 ms keeps added latency well below one burst.
    constexpr LONGLONG poll_interval_100ns = 2 * 10000;

    // The endpoint properties audio_control_t::find_device_id matches a sink
    // name against: PKEY_Device_FriendlyName, PKEY_Device_DeviceDesc and
    // PKEY_DeviceInterface_FriendlyName. Local copies, since audio.cpp defines
    // the shared ones.
    constexpr PROPERTYKEY endpoint_name_keys[] = {
      {{0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 14},
      {{0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 2},
      {{0x026e516e, 0xb814, 0x414b, {0x83, 0xcd, 0x85, 0x6d, 0x6f, 0xef, 0x48, 0x22}}, 2},
    };

    // CLSID_MMDeviceEnumerator and IID_IMMDeviceEnumerator, also local so this
    // file links on its own (the tests build it without audio.cpp).
    constexpr CLSID mm_device_enumerator_clsid = {0xbcde0395, 0xe52f, 0x467c, {0x8e, 0x3d, 0xc4, 0x57, 0x92, 0x91, 0x69, 0x2e}};
    constexpr IID mm_device_enumerator_iid = {0xa95664d2, 0x9614, 0x4f35, {0xa7, 0x46, 0xde, 0x8d, 0xb6, 0x36, 0x17, 0xe6}};

    struct handle_closer {
      void operator()(HANDLE h) const {
        if (h) CloseHandle(h);
      }
    };
    using unique_handle = std::unique_ptr<std::remove_pointer_t<HANDLE>, handle_closer>;

    struct com_releaser {
      void operator()(IUnknown *p) const {
        if (p) p->Release();
      }
    };
    template<class T>
    using com_ptr = std::unique_ptr<T, com_releaser>;

    // endpoint_name is ASCII, so a per-character compare is exact.
    bool is_endpoint_name(const wchar_t *value) {
      if (!value) return false;
      std::size_t i = 0;
      for (; i < endpoint_name.size(); ++i) {
        if (value[i] != static_cast<wchar_t>(endpoint_name[i])) return false;
      }
      return value[i] == L'\0';
    }

    struct view_unmapper {
      void operator()(const void *p) const {
        if (p) UnmapViewOfFile(p);
      }
    };
    using unique_view = std::unique_ptr<const void, view_unmapper>;

    std::string describe(DWORD error) {
      switch (error) {
        case ERROR_FILE_NOT_FOUND:
          return "no tap section: the Atmos probe driver isn't loaded, or nothing has played to it yet";
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

  bool endpoint_present() {
    // Called from RTSP threads, which may not have COM set up. RPC_E_CHANGED_MODE
    // means it already is, in another apartment type, which works as is.
    const HRESULT init = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool uninit = SUCCEEDED(init);

    bool found = false;
    IMMDeviceEnumerator *raw_enumerator = nullptr;
    if (SUCCEEDED(CoCreateInstance(mm_device_enumerator_clsid, nullptr, CLSCTX_ALL, mm_device_enumerator_iid, reinterpret_cast<void **>(&raw_enumerator)))) {
      com_ptr<IMMDeviceEnumerator> enumerator {raw_enumerator};
      IMMDeviceCollection *raw_collection = nullptr;
      if (SUCCEEDED(enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &raw_collection))) {
        com_ptr<IMMDeviceCollection> collection {raw_collection};
        UINT count = 0;
        collection->GetCount(&count);
        for (UINT i = 0; i < count && !found; ++i) {
          IMMDevice *raw_device = nullptr;
          if (FAILED(collection->Item(i, &raw_device))) continue;
          com_ptr<IMMDevice> device {raw_device};

          IPropertyStore *raw_props = nullptr;
          if (FAILED(device->OpenPropertyStore(STGM_READ, &raw_props))) continue;
          com_ptr<IPropertyStore> props {raw_props};

          for (const auto &key : endpoint_name_keys) {
            PROPVARIANT value;
            PropVariantInit(&value);
            if (SUCCEEDED(props->GetValue(key, &value)) && value.vt == VT_LPWSTR && is_endpoint_name(value.pwszVal)) {
              found = true;
            }
            PropVariantClear(&value);
            if (found) break;
          }
        }
      }
    }

    if (uninit) CoUninitialize();
    return found;
  }

  std::unique_ptr<capture_t> capture_t::open(std::string &error, bool *not_created) {
    if (not_created) *not_created = false;
    auto impl = std::make_unique<impl_t>();
    impl->mapping.reset(OpenFileMappingW(FILE_MAP_READ, FALSE, section_name));
    if (!impl->mapping) {
      const DWORD code = GetLastError();
      if (not_created) *not_created = code == ERROR_FILE_NOT_FOUND;
      error = describe(code);
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
