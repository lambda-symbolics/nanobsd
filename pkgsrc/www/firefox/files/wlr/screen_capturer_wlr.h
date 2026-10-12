/*
 *  LispBSD: screen capture for wlroots compositors through the
 *  wlr-screencopy protocol, with no portal and no PipeWire.  One output is
 *  one source; every CaptureFrame() asks the compositor for a copy of it
 *  (cursor overlaid) into a wl_shm buffer and hands the pixels to libwebrtc.
 */

#ifndef MODULES_DESKTOP_CAPTURE_LINUX_WAYLAND_WLR_SCREEN_CAPTURER_WLR_H_
#define MODULES_DESKTOP_CAPTURE_LINUX_WAYLAND_WLR_SCREEN_CAPTURER_WLR_H_

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "modules/desktop_capture/desktop_capture_options.h"
#include "modules/desktop_capture/desktop_capturer.h"
#include "modules/desktop_capture/desktop_region.h"

struct wl_display;
struct wl_registry;
struct wl_shm;
struct wl_output;
struct wl_buffer;
struct zwlr_screencopy_manager_v1;
struct zwlr_screencopy_frame_v1;

namespace webrtc {

class ScreenCapturerWlr : public DesktopCapturer {
 public:
  // True when WAYLAND_DISPLAY points at a compositor offering
  // zwlr_screencopy_manager_v1.
  static bool IsSupported();
  static std::unique_ptr<DesktopCapturer> CreateRawScreenCapturer(
      const DesktopCaptureOptions& options);

  ScreenCapturerWlr();
  ~ScreenCapturerWlr() override;

  ScreenCapturerWlr(const ScreenCapturerWlr&) = delete;
  ScreenCapturerWlr& operator=(const ScreenCapturerWlr&) = delete;

  // DesktopCapturer interface.
  void Start(Callback* callback) override;
  void CaptureFrame() override;
  bool GetSourceList(SourceList* sources) override;
  bool SelectSource(SourceId id) override;

  // Wayland event plumbing (public so the C listeners can reach it).
  struct Output {
    wl_output* output = nullptr;
    uint32_t global = 0;
    std::string name;
    std::string description;
    int32_t width = 0;
    int32_t height = 0;
  };
  void OnGlobal(uint32_t name, const char* interface, uint32_t version);
  void OnGlobalRemove(uint32_t name);
  void OnOutputMode(wl_output* output, uint32_t flags, int32_t w, int32_t h);
  void OnOutputName(wl_output* output, const char* name);
  void OnOutputDescription(wl_output* output, const char* description);
  void OnFrameBuffer(uint32_t format, uint32_t width, uint32_t height,
                     uint32_t stride);
  void OnFrameFlags(uint32_t flags);
  void OnFrameReady();
  void OnFrameFailed();
  void OnFrameDamage(uint32_t x, uint32_t y, uint32_t w, uint32_t h);
  void OnFrameBufferDone();

 private:
  bool Init();
  bool Roundtrip();
  bool DispatchOnce(int timeout_ms);
  bool EnsureBuffer(uint32_t format, uint32_t width, uint32_t height,
                    uint32_t stride);
  void DestroyBuffer();
  void RequestCopy();
  Output* FindOutput(wl_output* output);

  wl_display* display_ = nullptr;
  wl_registry* registry_ = nullptr;
  wl_shm* shm_ = nullptr;
  zwlr_screencopy_manager_v1* manager_ = nullptr;
  uint32_t manager_version_ = 0;
  std::vector<Output> outputs_;
  size_t selected_ = 0;

  // The one shm buffer, reused while the compositor keeps asking for the
  // same format and size.
  wl_buffer* buffer_ = nullptr;
  void* buffer_data_ = nullptr;
  size_t buffer_size_ = 0;
  uint32_t buffer_format_ = 0;
  uint32_t buffer_width_ = 0;
  uint32_t buffer_height_ = 0;
  uint32_t buffer_stride_ = 0;

  // State of the capture in flight.
  zwlr_screencopy_frame_v1* frame_ = nullptr;
  bool frame_buffer_known_ = false;
  bool frame_copy_requested_ = false;
  bool frame_ready_ = false;
  bool frame_failed_ = false;
  bool frame_y_invert_ = false;
  uint32_t frame_format_ = 0;
  uint32_t frame_width_ = 0;
  uint32_t frame_height_ = 0;
  uint32_t frame_stride_ = 0;
  DesktopRegion frame_damage_;
  bool frame_has_damage_ = false;
  bool first_frame_ = true;

  Callback* callback_ = nullptr;
};

}  // namespace webrtc

#endif  // MODULES_DESKTOP_CAPTURE_LINUX_WAYLAND_WLR_SCREEN_CAPTURER_WLR_H_
