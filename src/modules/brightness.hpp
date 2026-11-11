#pragma once

#include "modules/module.hpp"

#include <gio/gio.h>
#include <glibmm/main.h>
#include <gtkmm/eventcontrollerscroll.h>
#include <gtkmm/label.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <string>

class BrightnessModule : public Module {
public:
  BrightnessModule() : Module("brightness") {
    icon_.set_text(kIcon);
    icon_.add_css_class("brightness-icon");
    value_.add_css_class("brightness-value");

    append(icon_);
    append(value_);

    auto scroll = Gtk::EventControllerScroll::create();
    scroll->set_flags(Gtk::EventControllerScroll::Flags::VERTICAL);
    scroll->signal_scroll().connect(
        [this](double, double dy) {
          step(dy < 0 ? kStep : -kStep);
          return true;
        },
        false);
    add_controller(scroll);

    find_device();
    update();

    track(Glib::signal_timeout().connect(
        sigc::mem_fun(*this, &BrightnessModule::update), 2000));
  }

private:
  static constexpr int kStep = 5;
  static constexpr const char *kIcon = "\U000F00DF";

  Gtk::Label icon_;
  Gtk::Label value_;

  std::string name_, dir_;
  int max_ = 0;
  int cur_ = 0;

  static int read_int(const std::string &path) {
    std::ifstream f(path);
    int v = -1;
    f >> v;
    return f ? v : -1;
  }

  void find_device() {
    namespace fs = std::filesystem;
    std::error_code ec;
    for (auto &de : fs::directory_iterator("/sys/class/backlight", ec)) {
      const int max = read_int((de.path() / "max_brightness").string());
      if (max > 0) {
        name_ = de.path().filename().string();
        dir_ = de.path().string();
        max_ = max;
        return;
      }
    }
  }

  int percent() const {
    return static_cast<int>(std::lround(100.0 * cur_ / max_));
  }

  void render() {
    value_.set_text(std::to_string(percent()) + "%");
    set_tooltip_text(name_);
  }

  bool update() {
    if (max_ <= 0) {
      set_visible(false);
      return true;
    }
    const int v = read_int(dir_ + "/brightness");
    if (v >= 0) {
      cur_ = v;
      set_visible(true);
      render();
    }
    return true;
  }

  void step(int delta_pct) {
    if (max_ <= 0)
      return;

    const int next = std::clamp(percent() + delta_pct, 1, 100);
    cur_ = static_cast<int>(std::lround(next * max_ / 100.0));
    render();

    GDBusConnection *bus = g_bus_get_sync(G_BUS_TYPE_SYSTEM, nullptr, nullptr);
    if (!bus)
      return;
    g_dbus_connection_call(
        bus, "org.freedesktop.login1", "/org/freedesktop/login1/session/auto",
        "org.freedesktop.login1.Session", "SetBrightness",
        g_variant_new("(ssu)", "backlight", name_.c_str(),
                      static_cast<guint32>(cur_)),
        nullptr, G_DBUS_CALL_FLAGS_NONE, -1, nullptr, nullptr, nullptr);
    g_object_unref(bus);
  }
};
