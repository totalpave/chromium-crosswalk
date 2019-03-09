// Copyright 2015 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "components/exo/wayland/server.h"

#include <alpha-compositing-unstable-v1-server-protocol.h>
#include <aura-shell-server-protocol.h>
#include <cursor-shapes-unstable-v1-server-protocol.h>
#include <gaming-input-unstable-v1-server-protocol.h>
#include <gaming-input-unstable-v2-server-protocol.h>
#include <grp.h>
#include <input-timestamps-unstable-v1-server-protocol.h>
#include <keyboard-configuration-unstable-v1-server-protocol.h>
#include <keyboard-extension-unstable-v1-server-protocol.h>
#include <linux-explicit-synchronization-unstable-v1-server-protocol.h>
#include <notification-shell-unstable-v1-server-protocol.h>
#include <pointer-gestures-unstable-v1-server-protocol.h>
#include <presentation-time-server-protocol.h>
#include <relative-pointer-unstable-v1-server-protocol.h>
#include <remote-shell-unstable-v1-server-protocol.h>
#include <secure-output-unstable-v1-server-protocol.h>
#include <stylus-tools-unstable-v1-server-protocol.h>
#include <stylus-unstable-v2-server-protocol.h>
#include <text-input-unstable-v1-server-protocol.h>
#include <viewporter-server-protocol.h>
#include <vsync-feedback-unstable-v1-server-protocol.h>
#include <wayland-server-core.h>
#include <wayland-server-protocol-core.h>
#include <xdg-shell-unstable-v6-server-protocol.h>

#include <memory>
#include <string>
#include <utility>

#include "base/command_line.h"
#include "base/files/file_path.h"
#include "components/exo/display.h"
#include "components/exo/wayland/wayland_display_output.h"
#include "components/exo/wayland/wl_compositor.h"
#include "components/exo/wayland/wl_data_device_manager.h"
#include "components/exo/wayland/wl_output.h"
#include "components/exo/wayland/wl_seat.h"
#include "components/exo/wayland/wl_shm.h"
#include "components/exo/wayland/wl_subcompositor.h"
#include "components/exo/wayland/wp_presentation.h"
#include "components/exo/wayland/wp_viewporter.h"
#include "components/exo/wayland/zcr_alpha_compositing.h"
#include "components/exo/wayland/zcr_secure_output.h"
#include "components/exo/wayland/zcr_stylus.h"
#include "components/exo/wayland/zcr_vsync_feedback.h"
#include "ui/display/display.h"
#include "ui/display/screen.h"

#if defined(OS_CHROMEOS)
#include "components/exo/wayland/wl_shell.h"
#include "components/exo/wayland/zaura_shell.h"
#include "components/exo/wayland/zcr_cursor_shapes.h"
#include "components/exo/wayland/zcr_gaming_input.h"
#include "components/exo/wayland/zcr_keyboard_configuration.h"
#include "components/exo/wayland/zcr_keyboard_extension.h"
#include "components/exo/wayland/zcr_notification_shell.h"
#include "components/exo/wayland/zcr_remote_shell.h"
#include "components/exo/wayland/zcr_stylus_tools.h"
#include "components/exo/wayland/zwp_input_timestamps_manager.h"
#include "components/exo/wayland/zwp_linux_explicit_synchronization.h"
#include "components/exo/wayland/zwp_pointer_gestures.h"
#include "components/exo/wayland/zwp_relative_pointer_manager.h"
#include "components/exo/wayland/zwp_text_input_manager.h"
#include "components/exo/wayland/zxdg_shell.h"
#endif

#if defined(USE_OZONE)
#include <linux-dmabuf-unstable-v1-server-protocol.h>
#include "components/exo/wayland/zwp_linux_dmabuf.h"
#endif

#if defined(USE_FULLSCREEN_SHELL)
#include <fullscreen-shell-unstable-v1-server-protocol.h>
#include "components/exo/wayland/zwp_fullscreen_shell.h"
#endif

namespace exo {
namespace wayland {
namespace switches {

// This flag can be used to override the default wayland socket name. It is
// useful when another wayland server is already running and using the
// default name.
constexpr char kWaylandServerSocket[] = "wayland-server-socket";
}

namespace {

// Default wayland socket name.
const base::FilePath::CharType kSocketName[] = FILE_PATH_LITERAL("wayland-0");

// Group used for wayland socket.
const char kWaylandSocketGroup[] = "wayland";

////////////////////////////////////////////////////////////////////////////////
// gaming_input_interface:

// Gamepad delegate class that forwards gamepad events to the client resource.
class WaylandGamepadDelegate : public GamepadDelegate {
 public:
  explicit WaylandGamepadDelegate(wl_resource* gamepad_resource)
      : gamepad_resource_(gamepad_resource) {}

  // Overridden from GamepadDelegate:
  void OnGamepadDestroying(Gamepad* gamepad) override { delete this; }
  bool CanAcceptGamepadEventsForSurface(Surface* surface) const override {
    wl_resource* surface_resource = GetSurfaceResource(surface);
    return surface_resource &&
           wl_resource_get_client(surface_resource) == client();
  }
  void OnStateChange(bool connected) override {
    uint32_t status = connected ? ZWP_GAMEPAD_V1_GAMEPAD_STATE_ON
                                : ZWP_GAMEPAD_V1_GAMEPAD_STATE_OFF;
    zwp_gamepad_v1_send_state_change(gamepad_resource_, status);
    wl_client_flush(client());
  }
  void OnAxis(int axis, double value) override {
    zwp_gamepad_v1_send_axis(gamepad_resource_, NowInMilliseconds(), axis,
                             wl_fixed_from_double(value));
  }
  void OnButton(int button, bool pressed, double value) override {
    uint32_t state = pressed ? ZWP_GAMEPAD_V1_BUTTON_STATE_PRESSED
                             : ZWP_GAMEPAD_V1_BUTTON_STATE_RELEASED;
    zwp_gamepad_v1_send_button(gamepad_resource_, NowInMilliseconds(), button,
                               state, wl_fixed_from_double(value));
  }
  void OnFrame() override {
    zwp_gamepad_v1_send_frame(gamepad_resource_, NowInMilliseconds());
    wl_client_flush(client());
  }

 private:
  // The client who own this gamepad instance.
  wl_client* client() const {
    return wl_resource_get_client(gamepad_resource_);
  }

  // The gamepad resource associated with the gamepad.
  wl_resource* const gamepad_resource_;

  DISALLOW_COPY_AND_ASSIGN(WaylandGamepadDelegate);
};

void gamepad_destroy(wl_client* client, wl_resource* resource) {
  wl_resource_destroy(resource);
}

const struct zwp_gamepad_v1_interface gamepad_implementation = {
    gamepad_destroy};

void gaming_input_get_gamepad(wl_client* client,
                              wl_resource* resource,
                              uint32_t id,
                              wl_resource* seat) {
  wl_resource* gamepad_resource = wl_resource_create(
      client, &zwp_gamepad_v1_interface, wl_resource_get_version(resource), id);

  base::Thread* gaming_input_thread = GetUserDataAs<base::Thread>(resource);

  SetImplementation(
      gamepad_resource, &gamepad_implementation,
      base::WrapUnique(new Gamepad(new WaylandGamepadDelegate(gamepad_resource),
                                   gaming_input_thread->task_runner().get())));
}

const struct zwp_gaming_input_v1_interface gaming_input_implementation = {
    gaming_input_get_gamepad};

void bind_gaming_input(wl_client* client,
                       void* data,
                       uint32_t version,
                       uint32_t id) {
  wl_resource* resource =
      wl_resource_create(client, &zwp_gaming_input_v1_interface, version, id);

  std::unique_ptr<base::Thread> gaming_input_thread(
      new base::Thread("Exo gaming input polling thread."));
  gaming_input_thread->StartWithOptions(
      base::Thread::Options(base::MessageLoop::TYPE_IO, 0));

  SetImplementation(resource, &gaming_input_implementation,
                    std::move(gaming_input_thread));
}

////////////////////////////////////////////////////////////////////////////////
// pointer_stylus interface:

class WaylandPointerStylusDelegate : public PointerStylusDelegate {
 public:
  WaylandPointerStylusDelegate(wl_resource* resource, Pointer* pointer)
      : resource_(resource), pointer_(pointer) {
    pointer_->SetStylusDelegate(this);
  }
  ~WaylandPointerStylusDelegate() override {
    if (pointer_ != nullptr)
      pointer_->SetStylusDelegate(nullptr);
  }
  void OnPointerDestroying(Pointer* pointer) override { pointer_ = nullptr; }
  void OnPointerToolChange(ui::EventPointerType type) override {
    uint wayland_type = ZWP_POINTER_STYLUS_V1_TOOL_TYPE_MOUSE;
    if (type == ui::EventPointerType::POINTER_TYPE_PEN)
      wayland_type = ZWP_POINTER_STYLUS_V1_TOOL_TYPE_PEN;
    zwp_pointer_stylus_v1_send_tool_change(resource_, wayland_type);
  }
  void OnPointerForce(base::TimeTicks time_stamp, float force) override {
    zwp_pointer_stylus_v1_send_force(resource_,
                                     TimeTicksToMilliseconds(time_stamp),
                                     wl_fixed_from_double(force));
  }
  void OnPointerTilt(base::TimeTicks time_stamp, gfx::Vector2dF tilt) override {
    zwp_pointer_stylus_v1_send_tilt(
        resource_, TimeTicksToMilliseconds(time_stamp),
        wl_fixed_from_double(tilt.x()), wl_fixed_from_double(tilt.y()));
  }

 private:
  wl_resource* resource_;
  Pointer* pointer_;

  // The client who own this pointer stylus instance.
  wl_client* client() const { return wl_resource_get_client(resource_); }

  DISALLOW_COPY_AND_ASSIGN(WaylandPointerStylusDelegate);
};

void pointer_stylus_destroy(wl_client* client, wl_resource* resource) {
  wl_resource_destroy(resource);
}

const struct zwp_pointer_stylus_v1_interface pointer_stylus_implementation = {
    pointer_stylus_destroy};

////////////////////////////////////////////////////////////////////////////////
// stylus interface:

void stylus_get_pointer_stylus(wl_client* client,
                               wl_resource* resource,
                               uint32_t id,
                               wl_resource* pointer_resource) {
  Pointer* pointer = GetUserDataAs<Pointer>(pointer_resource);

  wl_resource* stylus_resource =
      wl_resource_create(client, &zwp_pointer_stylus_v1_interface, 1, id);

  SetImplementation(stylus_resource, &pointer_stylus_implementation,
                    base::WrapUnique(new WaylandPointerStylusDelegate(
                        stylus_resource, pointer)));
}

const struct zwp_stylus_v1_interface stylus_implementation = {
    stylus_get_pointer_stylus};

void bind_stylus(wl_client* client, void* data, uint32_t version, uint32_t id) {
  wl_resource* resource =
      wl_resource_create(client, &zwp_stylus_v1_interface, version, id);
  wl_resource_set_implementation(resource, &stylus_implementation, data,
                                 nullptr);
}

}  // namespace

////////////////////////////////////////////////////////////////////////////////
// Server, public:

Server::Server(Display* display)
    : display_(display), wl_display_(wl_display_create()) {
  wl_global_create(wl_display_.get(), &wl_compositor_interface,
                   kWlCompositorVersion, display_, bind_compositor);
  wl_global_create(wl_display_.get(), &wl_shm_interface, 1, display_, bind_shm);
#if defined(USE_OZONE)
  wl_global_create(wl_display_.get(), &zwp_linux_dmabuf_v1_interface,
                   kZwpLinuxDmabufVersion, display_, bind_linux_dmabuf);
#endif
  wl_global_create(wl_display_.get(), &wl_subcompositor_interface, 1, display_,
                   bind_subcompositor);
  display::Screen::GetScreen()->AddObserver(this);
  for (const auto& display : display::Screen::GetScreen()->GetAllDisplays())
    OnDisplayAdded(display);
  wl_global_create(wl_display_.get(), &zcr_vsync_feedback_v1_interface, 1,
                   display_, bind_vsync_feedback);
  wl_global_create(wl_display_.get(), &wl_data_device_manager_interface,
                   kWlDataDeviceManagerVersion, display_,
                   bind_data_device_manager);
  wl_global_create(wl_display_.get(), &wp_viewporter_interface, 1, display_,
                   bind_viewporter);
  wl_global_create(wl_display_.get(), &wp_presentation_interface, 1, display_,
                   bind_presentation);
  wl_global_create(wl_display_.get(), &zcr_secure_output_v1_interface, 1,
                   display_, bind_secure_output);
  wl_global_create(wl_display_.get(), &zcr_alpha_compositing_v1_interface, 1,
                   display_, bind_alpha_compositing);
  wl_global_create(wl_display_.get(), &zcr_stylus_v2_interface, 1, display_,
                   bind_stylus_v2);
  wl_global_create(wl_display_.get(), &wl_seat_interface, kWlSeatVersion,
                   display_->seat(), bind_seat);
#if defined(OS_CHROMEOS)
  wl_global_create(wl_display_.get(), &wl_shell_interface, 1, display_,
                   bind_shell);
  wl_global_create(wl_display_.get(), &zaura_shell_interface,
                   kZAuraShellVersion, display_, bind_aura_shell);
  wl_global_create(wl_display_.get(), &zcr_cursor_shapes_v1_interface, 1,
                   display_, bind_cursor_shapes);
  wl_global_create(wl_display_.get(), &zcr_gaming_input_v2_interface, 1,
                   display_, bind_gaming_input);
  wl_global_create(wl_display_.get(), &zcr_keyboard_configuration_v1_interface,
                   kZcrKeyboardConfigurationVersion, display_,
                   bind_keyboard_configuration);
  wl_global_create(wl_display_.get(), &zcr_keyboard_extension_v1_interface, 1,
                   display_, bind_keyboard_extension);
  wl_global_create(wl_display_.get(), &zcr_notification_shell_v1_interface, 1,
                   display_, bind_notification_shell);
  wl_global_create(wl_display_.get(), &zcr_remote_shell_v1_interface,
                   kZcrRemoteShellVersion, display_, bind_remote_shell);
  wl_global_create(wl_display_.get(), &zcr_stylus_tools_v1_interface, 1,
                   display_, bind_stylus_tools);
  wl_global_create(wl_display_.get(),
                   &zwp_input_timestamps_manager_v1_interface, 1, display_,
                   bind_input_timestamps_manager);
  wl_global_create(wl_display_.get(), &zwp_pointer_gestures_v1_interface, 1,
                   display_, bind_pointer_gestures);
  wl_global_create(wl_display_.get(),
                   &zwp_relative_pointer_manager_v1_interface, 1, display_,
                   bind_relative_pointer_manager);
  wl_global_create(wl_display_.get(), &zwp_text_input_manager_v1_interface, 1,
                   display_, bind_text_input_manager);
  wl_global_create(wl_display_.get(), &zxdg_shell_v6_interface, 1, display_,
                   bind_xdg_shell_v6);
  wl_global_create(wl_display_.get(),
                   &zwp_linux_explicit_synchronization_v1_interface, 1,
                   display_, bind_linux_explicit_synchronization);
#endif

#if defined(USE_FULLSCREEN_SHELL)
  wl_global_create(wl_display_.get(), &zwp_fullscreen_shell_v1_interface, 1,
                   display_, bind_fullscreen_shell);
#endif
}

Server::~Server() {
  display::Screen::GetScreen()->RemoveObserver(this);
}

// static
std::unique_ptr<Server> Server::Create(Display* display) {
  std::unique_ptr<Server> server(new Server(display));

  char* runtime_dir = getenv("XDG_RUNTIME_DIR");
  if (!runtime_dir) {
    LOG(ERROR) << "XDG_RUNTIME_DIR not set in the environment";
    return nullptr;
  }

  std::string socket_name(kSocketName);
  base::CommandLine* command_line = base::CommandLine::ForCurrentProcess();
  if (command_line->HasSwitch(switches::kWaylandServerSocket)) {
    socket_name =
        command_line->GetSwitchValueASCII(switches::kWaylandServerSocket);
  }

  if (!server->AddSocket(socket_name.c_str())) {
    LOG(ERROR) << "Failed to add socket: " << socket_name;
    return nullptr;
  }

  base::FilePath socket_path = base::FilePath(runtime_dir).Append(socket_name);

  // Change permissions on the socket.
  struct group wayland_group;
  struct group* wayland_group_res = nullptr;
  char buf[10000];
  if (HANDLE_EINTR(getgrnam_r(kWaylandSocketGroup, &wayland_group, buf,
                              sizeof(buf), &wayland_group_res)) < 0) {
    PLOG(ERROR) << "getgrnam_r";
    return nullptr;
  }
  if (wayland_group_res) {
    if (HANDLE_EINTR(chown(socket_path.MaybeAsASCII().c_str(), -1,
                           wayland_group.gr_gid)) < 0) {
      PLOG(ERROR) << "chown";
      return nullptr;
    }
  } else {
    LOG(WARNING) << "Group '" << kWaylandSocketGroup << "' not found";
  }

  if (!base::SetPosixFilePermissions(socket_path, 0660)) {
    PLOG(ERROR) << "Could not set permissions: " << socket_path.value();
    return nullptr;
  }

  return server;
}

bool Server::AddSocket(const std::string name) {
  DCHECK(!name.empty());
  return !wl_display_add_socket(wl_display_.get(), name.c_str());
}

int Server::GetFileDescriptor() const {
  wl_event_loop* event_loop = wl_display_get_event_loop(wl_display_.get());
  DCHECK(event_loop);
  return wl_event_loop_get_fd(event_loop);
}

void Server::Dispatch(base::TimeDelta timeout) {
  wl_event_loop* event_loop = wl_display_get_event_loop(wl_display_.get());
  DCHECK(event_loop);
  wl_event_loop_dispatch(event_loop, timeout.InMilliseconds());
}

void Server::Flush() {
  wl_display_flush_clients(wl_display_.get());
}

void Server::OnDisplayAdded(const display::Display& new_display) {
  auto output = std::make_unique<WaylandDisplayOutput>(new_display.id());
  output->set_global(wl_global_create(wl_display_.get(), &wl_output_interface,
                                      kWlOutputVersion, output.get(),
                                      bind_output));
  DCHECK_EQ(outputs_.count(new_display.id()), 0u);
  outputs_.insert(std::make_pair(new_display.id(), std::move(output)));
}

void Server::OnDisplayRemoved(const display::Display& old_display) {
  DCHECK_EQ(outputs_.count(old_display.id()), 1u);
  outputs_.erase(old_display.id());
}

}  // namespace wayland
}  // namespace exo
