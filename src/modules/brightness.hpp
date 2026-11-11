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

  gint64 hold_until_ = 0;

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

  static int type_rank(const std::string &dir) {
    std::ifstream f(dir + "/type");
    std::string t;
    f >> t;
    if (t == "firmware")
      return 0;
    if (t == "platform")
      return 1;
    return 2;
  }

  void find_device() {
    namespace fs = std::filesystem;
    std::error_code ec;
    int best = 99;
    for (auto &de : fs::directory_iterator("/sys/class/backlight", ec)) {
      const int max = read_int((de.path() / "max_brightness").string());
      if (max <= 0)
        continue;
      const int rank = type_rank(de.path().string());
      if (rank < best) {
        best = rank;
        name_ = de.path().filename().string();
        dir_ = de.path().string();
        max_ = max;
      }
    }
    if (max_ > 0)
      g_message("brightness: устройство '%s', max=%d", name_.c_str(), max_);
  }

  struct Job {
    std::string name;
    guint32 value;
  };

  static void fallback_brightnessctl(const Job &job) {
    const std::string val = std::to_string(job.value);
    const char *argv[] = {"brightnessctl", "-d",        job.name.c_str(),
                          "set",           val.c_str(), nullptr};
    GError *err = nullptr;
    GSubprocess *p = g_subprocess_newv(argv, G_SUBPROCESS_FLAGS_NONE, &err);
    if (!p) {
      g_warning("brightness: brightnessctl не запустился: %s",
                err ? err->message : "?");
      g_clear_error(&err);
      return;
    }
    g_object_unref(p);
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
    if (g_get_monotonic_time() < hold_until_)
      return true;
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
    hold_until_ = g_get_monotonic_time() + 1500000;
    render();

    GDBusConnection *bus = g_bus_get_sync(G_BUS_TYPE_SYSTEM, nullptr, nullptr);
    auto *job = new Job{name_, static_cast<guint32>(cur_)};
    if (!bus) {
      fallback_brightnessctl(*job);
      delete job;
      return;
    }

    g_dbus_connection_call(
        bus, "org.freedesktop.login1", "/org/freedesktop/login1/session/auto",
        "org.freedesktop.login1.Session", "SetBrightness",
        g_variant_new("(ssu)", "backlight", job->name.c_str(), job->value),
        nullptr, G_DBUS_CALL_FLAGS_NONE, -1, nullptr,
        +[](GObject *src, GAsyncResult *res, gpointer data) {
          std::unique_ptr<Job> job(static_cast<Job *>(data));
          GError *err = nullptr;
          GVariant *r =
              g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, &err);
          if (r) {
            g_variant_unref(r);
            return;
          }
          g_warning("brightness: logind SetBrightness: %s",
                    err ? err->message : "?");
          g_clear_error(&err);
          fallback_brightnessctl(*job);
        },
        job);
    g_object_unref(bus);
  }
};
