#pragma once
#include <glibmm.h>
#include <sigc++/sigc++.h>

#include <string>

class HyprlandIPC {
public:
  HyprlandIPC() = default;
  ~HyprlandIPC() { disconnect(); }

  HyprlandIPC(const HyprlandIPC &) = delete;
  HyprlandIPC &operator=(const HyprlandIPC &) = delete;

  bool connect();
  void disconnect();

  void dispatch_workspace(int id);
  void toggle_noscreenshare();
  std::string request(const std::string &command);

  sigc::signal<void(int, const std::string &)> &signal_workspace_changed() {
    return workspace_changed_;
  }
  sigc::signal<void(const std::string &)> &signal_active_window_changed() {
    return active_window_changed_;
  }

  sigc::signal<void(const std::string &, const std::string &)> &signal_event() {
    return event_;
  }

  sigc::signal<void(const std::string &, const std::string &)> &
  signal_active_layout_changed() {
    return active_layout_changed_;
  }

  sigc::signal<void()> &signal_tags_changed() { return tags_changed_; }

private:
  sigc::signal<void(int, const std::string &)> workspace_changed_;
  sigc::signal<void(const std::string &)> active_window_changed_;
  sigc::signal<void(const std::string &, const std::string &)> event_;
  sigc::signal<void(const std::string &, const std::string &)>
      active_layout_changed_;

  sigc::signal<void()> tags_changed_;

  int event_fd_ = -1;
  sigc::connection io_conn_;
  std::string buffer_;

  static std::string socket_path(const char *name);
  bool on_event_data(Glib::IOCondition cond);
  void handle_line(const std::string &line);
};
