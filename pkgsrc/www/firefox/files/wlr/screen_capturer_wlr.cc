/*
 *  LispBSD: screen capture for wlroots compositors through wlr-screencopy.
 *  See screen_capturer_wlr.h.
 */

#include "modules/desktop_capture/linux/wayland_wlr/screen_capturer_wlr.h"

#include <poll.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>

#include <cstdlib>
#include <cstring>
#include <memory>
#include <utility>

#include "modules/desktop_capture/desktop_capture_types.h"
#include "modules/desktop_capture/desktop_frame.h"
#include "modules/desktop_capture/desktop_geometry.h"
#include "modules/desktop_capture/linux/wayland_wlr/wlr-screencopy-unstable-v1-client-protocol.h"
#include "rtc_base/logging.h"
#include "rtc_base/time_utils.h"

namespace webrtc {

namespace {

constexpr int kRoundtripTimeoutMs = 2000;
constexpr int kFrameTimeoutMs = 1500;

// --- wl_registry ---------------------------------------------------------

void registry_global(void* data, wl_registry*, uint32_t name,
                     const char* interface, uint32_t version) {
  static_cast<ScreenCapturerWlr*>(data)->OnGlobal(name, interface, version);
}

void registry_global_remove(void* data, wl_registry*, uint32_t name) {
  static_cast<ScreenCapturerWlr*>(data)->OnGlobalRemove(name);
}

const wl_registry_listener kRegistryListener = {
    registry_global,
    registry_global_remove,
};

// --- wl_output -----------------------------------------------------------

void output_geometry(void*, wl_output*, int32_t, int32_t, int32_t, int32_t,
                     int32_t, const char*, const char*, int32_t) {}

void output_mode(void* data, wl_output* output, uint32_t flags, int32_t w,
                 int32_t h, int32_t) {
  static_cast<ScreenCapturerWlr*>(data)->OnOutputMode(output, flags, w, h);
}

void output_done(void*, wl_output*) {}

void output_scale(void*, wl_output*, int32_t) {}

void output_name(void* data, wl_output* output, const char* name) {
  static_cast<ScreenCapturerWlr*>(data)->OnOutputName(output, name);
}

void output_description(void* data, wl_output* output, const char* desc) {
  static_cast<ScreenCapturerWlr*>(data)->OnOutputDescription(output, desc);
}

const wl_output_listener kOutputListener = {
    output_geometry, output_mode, output_done,
    output_scale,    output_name, output_description,
};

// --- zwlr_screencopy_frame_v1 -------------------------------------------

void frame_buffer(void* data, zwlr_screencopy_frame_v1*, uint32_t format,
                  uint32_t width, uint32_t height, uint32_t stride) {
  static_cast<ScreenCapturerWlr*>(data)->OnFrameBuffer(format, width, height,
                                                        stride);
}

void frame_flags(void* data, zwlr_screencopy_frame_v1*, uint32_t flags) {
  static_cast<ScreenCapturerWlr*>(data)->OnFrameFlags(flags);
}

void frame_ready(void* data, zwlr_screencopy_frame_v1*, uint32_t, uint32_t,
                 uint32_t) {
  static_cast<ScreenCapturerWlr*>(data)->OnFrameReady();
}

void frame_failed(void* data, zwlr_screencopy_frame_v1*) {
  static_cast<ScreenCapturerWlr*>(data)->OnFrameFailed();
}

void frame_damage(void* data, zwlr_screencopy_frame_v1*, uint32_t x,
                  uint32_t y, uint32_t w, uint32_t h) {
  static_cast<ScreenCapturerWlr*>(data)->OnFrameDamage(x, y, w, h);
}

void frame_linux_dmabuf(void*, zwlr_screencopy_frame_v1*, uint32_t, uint32_t,
                        uint32_t) {}

void frame_buffer_done(void* data, zwlr_screencopy_frame_v1*) {
  static_cast<ScreenCapturerWlr*>(data)->OnFrameBufferDone();
}

const zwlr_screencopy_frame_v1_listener kFrameListener = {
    frame_buffer, frame_flags,        frame_ready,       frame_failed,
    frame_damage, frame_linux_dmabuf, frame_buffer_done,
};

bool FormatIsBgra(uint32_t format) {
  return format == WL_SHM_FORMAT_XRGB8888 || format == WL_SHM_FORMAT_ARGB8888;
}

bool FormatIsRgba(uint32_t format) {
  return format == WL_SHM_FORMAT_XBGR8888 || format == WL_SHM_FORMAT_ABGR8888;
}

}  // namespace

// static
bool ScreenCapturerWlr::IsSupported() {
  if (!getenv("WAYLAND_DISPLAY")) {
    return false;
  }
  ScreenCapturerWlr probe;
  bool ok = probe.Init();
  RTC_LOG(LS_INFO) << "ScreenCapturerWlr: wlr-screencopy "
                   << (ok ? "available" : "not available");
  return ok;
}

// static
std::unique_ptr<DesktopCapturer> ScreenCapturerWlr::CreateRawScreenCapturer(
    const DesktopCaptureOptions& options) {
  auto capturer = std::make_unique<ScreenCapturerWlr>();
  if (!capturer->Init()) {
    return nullptr;
  }
  return capturer;
}

ScreenCapturerWlr::ScreenCapturerWlr() = default;

ScreenCapturerWlr::~ScreenCapturerWlr() {
  if (frame_) {
    zwlr_screencopy_frame_v1_destroy(frame_);
  }
  DestroyBuffer();
  for (auto& o : outputs_) {
    if (o.output) {
      wl_output_destroy(o.output);
    }
  }
  if (manager_) {
    zwlr_screencopy_manager_v1_destroy(manager_);
  }
  if (shm_) {
    wl_shm_destroy(shm_);
  }
  if (registry_) {
    wl_registry_destroy(registry_);
  }
  if (display_) {
    wl_display_disconnect(display_);
  }
}

bool ScreenCapturerWlr::Init() {
  display_ = wl_display_connect(nullptr);
  if (!display_) {
    RTC_LOG(LS_WARNING) << "ScreenCapturerWlr: no Wayland display";
    return false;
  }
  registry_ = wl_display_get_registry(display_);
  wl_registry_add_listener(registry_, &kRegistryListener, this);
  // One roundtrip lists the globals, a second one brings the outputs'
  // modes and names in.
  if (!Roundtrip() || !Roundtrip()) {
    RTC_LOG(LS_WARNING) << "ScreenCapturerWlr: registry roundtrip failed";
    return false;
  }
  if (!manager_ || !shm_ || outputs_.empty()) {
    RTC_LOG(LS_INFO) << "ScreenCapturerWlr: compositor lacks "
                     << (manager_ ? "" : "zwlr_screencopy_manager_v1 ")
                     << (shm_ ? "" : "wl_shm ")
                     << (outputs_.empty() ? "outputs" : "");
    return false;
  }
  return true;
}

bool ScreenCapturerWlr::Roundtrip() {
  wl_callback* cb = wl_display_sync(display_);
  bool done = false;
  static const wl_callback_listener listener = {
      [](void* data, wl_callback* cb, uint32_t) {
        *static_cast<bool*>(data) = true;
        wl_callback_destroy(cb);
      }};
  wl_callback_add_listener(cb, &listener, &done);
  int64_t deadline = TimeMillis() + kRoundtripTimeoutMs;
  while (!done) {
    int left = static_cast<int>(deadline - TimeMillis());
    if (left <= 0 || !DispatchOnce(left)) {
      return false;
    }
  }
  return true;
}

// Read and dispatch what the compositor has to say, waiting at most
// timeout_ms for it; false on timeout or a dead connection.
bool ScreenCapturerWlr::DispatchOnce(int timeout_ms) {
  while (wl_display_prepare_read(display_) != 0) {
    if (wl_display_dispatch_pending(display_) < 0) {
      return false;
    }
  }
  if (wl_display_flush(display_) < 0 && errno != EAGAIN) {
    wl_display_cancel_read(display_);
    return false;
  }
  struct pollfd pfd = {wl_display_get_fd(display_), POLLIN, 0};
  int r = poll(&pfd, 1, timeout_ms);
  if (r <= 0) {
    wl_display_cancel_read(display_);
    return false;
  }
  if (wl_display_read_events(display_) < 0) {
    return false;
  }
  return wl_display_dispatch_pending(display_) >= 0;
}

void ScreenCapturerWlr::OnGlobal(uint32_t name, const char* interface,
                                 uint32_t version) {
  if (strcmp(interface, zwlr_screencopy_manager_v1_interface.name) == 0) {
    manager_version_ = version < 3 ? version : 3;
    manager_ = static_cast<zwlr_screencopy_manager_v1*>(wl_registry_bind(
        registry_, name, &zwlr_screencopy_manager_v1_interface,
        manager_version_));
  } else if (strcmp(interface, wl_shm_interface.name) == 0) {
    shm_ = static_cast<wl_shm*>(
        wl_registry_bind(registry_, name, &wl_shm_interface, 1));
  } else if (strcmp(interface, wl_output_interface.name) == 0) {
    Output o;
    o.global = name;
    o.output = static_cast<wl_output*>(wl_registry_bind(
        registry_, name, &wl_output_interface, version < 4 ? version : 4));
    wl_output_add_listener(o.output, &kOutputListener, this);
    outputs_.push_back(o);
  }
}

void ScreenCapturerWlr::OnGlobalRemove(uint32_t name) {
  for (size_t i = 0; i < outputs_.size(); i++) {
    if (outputs_[i].global == name) {
      wl_output_destroy(outputs_[i].output);
      outputs_.erase(outputs_.begin() + i);
      if (selected_ >= outputs_.size()) {
        selected_ = 0;
      }
      return;
    }
  }
}

ScreenCapturerWlr::Output* ScreenCapturerWlr::FindOutput(wl_output* output) {
  for (auto& o : outputs_) {
    if (o.output == output) {
      return &o;
    }
  }
  return nullptr;
}

void ScreenCapturerWlr::OnOutputMode(wl_output* output, uint32_t flags,
                                     int32_t w, int32_t h) {
  Output* o = FindOutput(output);
  if (o && (flags & WL_OUTPUT_MODE_CURRENT)) {
    o->width = w;
    o->height = h;
  }
}

void ScreenCapturerWlr::OnOutputName(wl_output* output, const char* name) {
  if (Output* o = FindOutput(output)) {
    o->name = name;
  }
}

void ScreenCapturerWlr::OnOutputDescription(wl_output* output,
                                            const char* description) {
  if (Output* o = FindOutput(output)) {
    o->description = description;
  }
}

void ScreenCapturerWlr::Start(Callback* callback) {
  callback_ = callback;
}

bool ScreenCapturerWlr::GetSourceList(SourceList* sources) {
  for (size_t i = 0; i < outputs_.size(); i++) {
    Source s;
    s.id = static_cast<SourceId>(i);
    s.title = outputs_[i].name.empty() ? "Screen " + std::to_string(i + 1)
                                       : outputs_[i].name;
    if (!outputs_[i].description.empty()) {
      s.title += " (" + outputs_[i].description + ")";
    }
    sources->push_back(s);
  }
  return true;
}

bool ScreenCapturerWlr::SelectSource(SourceId id) {
  if (id == kFullDesktopScreenId) {
    selected_ = 0;
    return true;
  }
  if (id < 0 || static_cast<size_t>(id) >= outputs_.size()) {
    return false;
  }
  selected_ = static_cast<size_t>(id);
  first_frame_ = true;
  return true;
}

// --- the capture --------------------------------------------------------

void ScreenCapturerWlr::OnFrameBuffer(uint32_t format, uint32_t width,
                                      uint32_t height, uint32_t stride) {
  // Version 3 compositors list several buffer types and finish with
  // buffer_done; older ones send exactly one buffer event, so copy now.
  // Keep the first shm format offered.
  if (frame_buffer_known_) {
    return;
  }
  frame_buffer_known_ = true;
  frame_format_ = format;
  frame_width_ = width;
  frame_height_ = height;
  frame_stride_ = stride;
  if (manager_version_ < 3) {
    RequestCopy();
  }
}

void ScreenCapturerWlr::OnFrameBufferDone() {
  RequestCopy();
}

void ScreenCapturerWlr::RequestCopy() {
  if (frame_copy_requested_ || !frame_ || !frame_buffer_known_) {
    return;
  }
  frame_copy_requested_ = true;
  if (!EnsureBuffer(frame_format_, frame_width_, frame_height_,
                    frame_stride_)) {
    frame_failed_ = true;
    return;
  }
  if (manager_version_ >= 2 && !first_frame_) {
    zwlr_screencopy_frame_v1_copy_with_damage(frame_, buffer_);
  } else {
    zwlr_screencopy_frame_v1_copy(frame_, buffer_);
  }
}

void ScreenCapturerWlr::OnFrameFlags(uint32_t flags) {
  frame_y_invert_ = (flags & ZWLR_SCREENCOPY_FRAME_V1_FLAGS_Y_INVERT) != 0;
}

void ScreenCapturerWlr::OnFrameDamage(uint32_t x, uint32_t y, uint32_t w,
                                      uint32_t h) {
  frame_damage_.AddRect(DesktopRect::MakeXYWH(x, y, w, h));
  frame_has_damage_ = true;
}

void ScreenCapturerWlr::OnFrameReady() {
  frame_ready_ = true;
}

void ScreenCapturerWlr::OnFrameFailed() {
  frame_failed_ = true;
}

bool ScreenCapturerWlr::EnsureBuffer(uint32_t format, uint32_t width,
                                     uint32_t height, uint32_t stride) {
  if (buffer_ && buffer_format_ == format && buffer_width_ == width &&
      buffer_height_ == height && buffer_stride_ == stride) {
    return true;
  }
  DestroyBuffer();
  size_t size = static_cast<size_t>(stride) * height;
  int fd = memfd_create("firefox-screencopy", MFD_CLOEXEC);
  if (fd < 0) {
    RTC_LOG(LS_ERROR) << "ScreenCapturerWlr: memfd_create failed";
    return false;
  }
  if (ftruncate(fd, static_cast<off_t>(size)) < 0) {
    close(fd);
    return false;
  }
  void* data = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (data == MAP_FAILED) {
    close(fd);
    RTC_LOG(LS_ERROR) << "ScreenCapturerWlr: mmap failed";
    return false;
  }
  wl_shm_pool* pool = wl_shm_create_pool(shm_, fd, static_cast<int32_t>(size));
  buffer_ = wl_shm_pool_create_buffer(pool, 0, static_cast<int32_t>(width),
                                      static_cast<int32_t>(height),
                                      static_cast<int32_t>(stride),
                                      format);
  wl_shm_pool_destroy(pool);
  close(fd);
  buffer_data_ = data;
  buffer_size_ = size;
  buffer_format_ = format;
  buffer_width_ = width;
  buffer_height_ = height;
  buffer_stride_ = stride;
  return buffer_ != nullptr;
}

void ScreenCapturerWlr::DestroyBuffer() {
  if (buffer_) {
    wl_buffer_destroy(buffer_);
    buffer_ = nullptr;
  }
  if (buffer_data_) {
    munmap(buffer_data_, buffer_size_);
    buffer_data_ = nullptr;
    buffer_size_ = 0;
  }
}

void ScreenCapturerWlr::CaptureFrame() {
  if (!callback_) {
    return;
  }
  if (!manager_ || selected_ >= outputs_.size()) {
    callback_->OnCaptureResult(Result::ERROR_PERMANENT, nullptr);
    return;
  }
  int64_t started = TimeMillis();

  frame_buffer_known_ = false;
  frame_copy_requested_ = false;
  frame_ready_ = false;
  frame_failed_ = false;
  frame_y_invert_ = false;
  frame_has_damage_ = false;
  frame_damage_.Clear();

  frame_ = zwlr_screencopy_manager_v1_capture_output(
      manager_, 1 /* overlay the cursor */, outputs_[selected_].output);
  zwlr_screencopy_frame_v1_add_listener(frame_, &kFrameListener, this);

  int64_t deadline = started + kFrameTimeoutMs;
  bool alive = true;
  while (!frame_ready_ && !frame_failed_) {
    int left = static_cast<int>(deadline - TimeMillis());
    if (left <= 0 || !DispatchOnce(left)) {
      alive = false;
      break;
    }
  }
  zwlr_screencopy_frame_v1_destroy(frame_);
  frame_ = nullptr;

  if (!alive) {
    RTC_LOG(LS_WARNING) << "ScreenCapturerWlr: no frame within "
                        << kFrameTimeoutMs << " ms";
    callback_->OnCaptureResult(Result::ERROR_TEMPORARY, nullptr);
    return;
  }
  if (frame_failed_ || !buffer_data_) {
    callback_->OnCaptureResult(Result::ERROR_TEMPORARY, nullptr);
    return;
  }
  if (!FormatIsBgra(buffer_format_) && !FormatIsRgba(buffer_format_)) {
    RTC_LOG(LS_ERROR) << "ScreenCapturerWlr: unsupported shm format "
                      << buffer_format_;
    callback_->OnCaptureResult(Result::ERROR_PERMANENT, nullptr);
    return;
  }

  // Into a frame of libwebrtc's own: BGRA, top-down.
  DesktopSize size(static_cast<int>(buffer_width_),
                   static_cast<int>(buffer_height_));
  auto frame = std::make_unique<BasicDesktopFrame>(size);
  const bool swap = FormatIsRgba(buffer_format_);
  for (uint32_t y = 0; y < buffer_height_; y++) {
    uint32_t src_y = frame_y_invert_ ? buffer_height_ - 1 - y : y;
    const uint8_t* src =
        static_cast<const uint8_t*>(buffer_data_) + src_y * buffer_stride_;
    uint8_t* dst = frame->data() + y * frame->stride();
    if (!swap) {
      memcpy(dst, src, buffer_width_ * DesktopFrame::kBytesPerPixel);
    } else {
      for (uint32_t x = 0; x < buffer_width_; x++) {
        dst[x * 4 + 0] = src[x * 4 + 2];
        dst[x * 4 + 1] = src[x * 4 + 1];
        dst[x * 4 + 2] = src[x * 4 + 0];
        dst[x * 4 + 3] = src[x * 4 + 3];
      }
    }
  }
  if (frame_has_damage_ && !first_frame_) {
    frame_damage_.IntersectWith(DesktopRect::MakeSize(size));
    *frame->mutable_updated_region() = frame_damage_;
  } else {
    frame->mutable_updated_region()->SetRect(DesktopRect::MakeSize(size));
  }
  first_frame_ = false;
  frame->set_capture_time_ms(TimeMillis() - started);
  callback_->OnCaptureResult(Result::SUCCESS, std::move(frame));
}

}  // namespace webrtc
