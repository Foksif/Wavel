#include "hyprland.hpp"

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>

namespace {

int open_unix_socket(const std::string &path) {
  int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0)
    return -1;

  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  if (path.size() >= sizeof(addr.sun_path)) {
    ::close(fd);
    return -1;
  }
  std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);

  if (::connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0) {
    ::close(fd);
    return -1;
  }
  return fd;
}

} // namespace

std::string HyprlandIPC::socket_path(const char *name) {
  const char *runtime = std::getenv("XDG_RUNTIME_DIR");
  const char *sig = std::getenv("HYPRLAND_INSTANCE_SIGNATURE");
  if (!runtime || !sig)
    return {};
  return std::string(runtime) + "/hypr/" + sig + "/" + name;
}

bool HyprlandIPC::connect() {
  if (event_fd_ >= 0)
    return true;

  const auto path = socket_path(".socket2.sock");
  if (path.empty()) {
    g_warning("Hyprland: HYPRLAND_INSTANCE_SIGNATURE не задан");
    return false;
  }

  event_fd_ = open_unix_socket(path);
  if (event_fd_ < 0) {
    g_warning("Hyprland: не удалось подключиться к %s: %s", path.c_str(),
              std::strerror(errno));
    return false;
  }

  io_conn_ = Glib::signal_io().connect(
      sigc::mem_fun(*this, &HyprlandIPC::on_event_data), event_fd_,
      Glib::IOCondition::IO_IN | Glib::IOCondition::IO_HUP |
          Glib::IOCondition::IO_ERR);
  return true;
}

void HyprlandIPC::disconnect() {
  io_conn_.disconnect();
  if (event_fd_ >= 0) {
    ::close(event_fd_);
    event_fd_ = -1;
  }
  buffer_.clear();
}

bool HyprlandIPC::on_event_data(Glib::IOCondition cond) {
  if ((cond & Glib::IOCondition::IO_HUP) != Glib::IOCondition{} ||
      (cond & Glib::IOCondition::IO_ERR) != Glib::IOCondition{}) {
    g_warning("Hyprland: сокет событий закрыт");
    disconnect();
    return false;
  }

  char chunk[4096];
  ssize_t n = ::read(event_fd_, chunk, sizeof(chunk));
  if (n <= 0) {
    if (n < 0 && (errno == EAGAIN || errno == EINTR))
      return true;
    disconnect();
    return false;
  }

  buffer_.append(chunk, static_cast<size_t>(n));

  size_t pos;
  while ((pos = buffer_.find('\n')) != std::string::npos) {
    handle_line(buffer_.substr(0, pos));
    buffer_.erase(0, pos + 1);
  }
  return true;
}

void HyprlandIPC::handle_line(const std::string &line) {
  const auto sep = line.find(">>");
  if (sep == std::string::npos)
    return;

  const std::string event = line.substr(0, sep);
  const std::string data = line.substr(sep + 2);

  if (event == "workspacev2") {
    const auto comma = data.find(',');
    if (comma == std::string::npos)
      return;
    int id = std::atoi(data.substr(0, comma).c_str());
    workspace_changed_.emit(id, data.substr(comma + 1));
  } else if (event == "activewindow") {
    const auto comma = data.find(',');
    active_window_changed_.emit(
        comma == std::string::npos ? data : data.substr(comma + 1));
  } else if (event == "activelayout") {
    const auto comma = data.find(',');
    if (comma == std::string::npos)
      return;

    active_layout_changed_.emit(data.substr(0, comma), data.substr(comma + 1));
  }
  event_.emit(event, data);
}

std::string HyprlandIPC::request(const std::string &command) {
  const auto path = socket_path(".socket.sock");
  if (path.empty())
    return {};

  int fd = open_unix_socket(path);
  if (fd < 0)
    return {};

  std::string reply;
  if (::write(fd, command.data(), command.size()) ==
      static_cast<ssize_t>(command.size())) {
    char chunk[4096];
    ssize_t n;
    while ((n = ::read(fd, chunk, sizeof(chunk))) > 0)
      reply.append(chunk, static_cast<size_t>(n));
  }

  ::close(fd);

  // g_message("IPC request: [%s]", command.c_str());
  // g_message("IPC response: [%s]", reply.c_str());

  return reply;
}

void HyprlandIPC::dispatch_workspace(int id) {
  request("dispatch hl.dsp.focus({ workspace = \"" + std::to_string(id) +
          "\" })");
}

void HyprlandIPC::toggle_noscreenshare() {
  request("dispatch hl.dsp.window.tag({ tag = 'noscreenshare' })");
  tags_changed_.emit();
}
