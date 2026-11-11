#include "ipc/hyprland.hpp"
#include "island.hpp"
#include "modules/activewindow.hpp"
#include "modules/audio.hpp"
#include "modules/battery.hpp"
#include "modules/keyboard.hpp"
#include "modules/tray.hpp"
#include "modules/workspaces.hpp"
#include "theme.hpp"
#include <csignal>
#include <glib-unix.h>
#include <gtk4-layer-shell.h>
#include <gtkmm.h>
#include <map>
#include <memory>

class WavelWindow : public Gtk::ApplicationWindow {
public:
  explicit WavelWindow(GdkMonitor *monitor, HyprlandIPC &ipc) : ipc_(ipc) {
    setup_layer_shell(monitor);
    setup_content();
  }

private:
  HyprlandIPC &ipc_;
  Gtk::CenterBox center_box_;
  Gtk::Box left_{Gtk::Orientation::HORIZONTAL, 8};
  Gtk::Box center_{Gtk::Orientation::HORIZONTAL, 8}; // TODO: Вырезать потом
  Gtk::Box right_{Gtk::Orientation::HORIZONTAL, 8};

  void setup_layer_shell(GdkMonitor *monitor) {
    GtkWindow *gw = GTK_WINDOW(gobj());

    gtk_layer_init_for_window(gw);
    gtk_layer_set_namespace(gw, "wavel");
    gtk_layer_set_monitor(gw, monitor);
    gtk_layer_set_layer(gw, GTK_LAYER_SHELL_LAYER_TOP);

    gtk_layer_set_anchor(gw, GTK_LAYER_SHELL_EDGE_TOP, true);
    gtk_layer_set_anchor(gw, GTK_LAYER_SHELL_EDGE_LEFT, true);
    gtk_layer_set_anchor(gw, GTK_LAYER_SHELL_EDGE_RIGHT, true);

    gtk_layer_auto_exclusive_zone_enable(gw);
    gtk_layer_set_keyboard_mode(gw, GTK_LAYER_SHELL_KEYBOARD_MODE_NONE);

    set_default_size(-1, 32);
    set_decorated(false);
  }

  void setup_content() {
    add_css_class("wavel-bar");
    left_.add_css_class("left");
    center_.add_css_class("center");
    right_.add_css_class("right");

    left_.append(*Gtk::make_managed<WorkspacesModule>(ipc_));
    left_.append(*Gtk::make_managed<ActiveWindowModule>(ipc_));

    // center_ не испольщуем так как там остров

    right_.append(*Gtk::make_managed<AudioModule>());
    right_.append(*Gtk::make_managed<KeyboardModule>(ipc_));
    right_.append(*Gtk::make_managed<BatteryModule>());
    right_.append(*Gtk::make_managed<TrayModule>());

    center_box_.set_start_widget(left_);
    center_box_.set_center_widget(center_);
    center_box_.set_end_widget(right_);
    set_child(center_box_);
  }
};

class WavelApplication : public Gtk::Application {
public:
  HyprlandIPC ipc_;
  WavelApplication() : Gtk::Application("io.github.foksif.wavel") {}

protected:
  void on_activate() override {
    themes_.init();
    ipc_.connect();

    GdkDisplay *display = gdk_display_get_default();
    monitors_ = gdk_display_get_monitors(display);

    g_signal_connect(
        monitors_, "items-changed",
        G_CALLBACK(+[](GListModel *, guint, guint, guint, gpointer data) {
          static_cast<WavelApplication *>(data)->sync_windows();
        }),
        this);

    mpris_.start();
    notifications_.start();
    sync_windows();

    g_unix_signal_add(
        SIGUSR1,
        +[](gpointer data) -> gboolean {
          static_cast<WavelApplication *>(data)->ipc_.toggle_noscreenshare();
          return G_SOURCE_CONTINUE;
        },
        this);
  }

private:
  ThemeManager themes_;
  GListModel *monitors_ = nullptr;
  std::map<GdkMonitor *, std::unique_ptr<WavelWindow>> windows_;
  std::map<GdkMonitor *, std::unique_ptr<IslandWindow>> islands_;
  MprisClient mpris_;
  NotificationServer notifications_;

  bool monitor_present(GdkMonitor *target, guint count) const {
    for (guint i = 0; i < count; ++i) {
      auto *m = GDK_MONITOR(g_list_model_get_item(monitors_, i));
      bool same = (m == target);
      g_object_unref(m);
      if (same)
        return true;
    }
    return false;
  }

  void sync_windows() {
    guint count = g_list_model_get_n_items(monitors_);

    for (auto it = windows_.begin(); it != windows_.end();) {
      if (!monitor_present(it->first, count)) {
        it->second->close();
        it = windows_.erase(it);
      } else {
        ++it;
      }
    }

    for (auto it = islands_.begin(); it != islands_.end();) {
      if (!monitor_present(it->first, count)) {
        it->second->close();
        it = islands_.erase(it);
      } else {
        ++it;
      }
    }

    for (guint i = 0; i < count; ++i) {
      auto *monitor = GDK_MONITOR(g_list_model_get_item(monitors_, i));

      if (!windows_.count(monitor)) {
        auto window = std::make_unique<WavelWindow>(monitor, ipc_);
        add_window(*window);
        window->present();
        windows_[monitor] = std::move(window);
      }

      if (!islands_.count(monitor)) {
        auto island = std::make_unique<IslandWindow>(monitor, mpris_,
                                                     notifications_, themes_);
        add_window(*island);
        island->present();
        islands_[monitor] = std::move(island);
      }

      g_object_unref(monitor);
    }
  }
};

int main(int argc, char *argv[]) { return WavelApplication().run(argc, argv); }
