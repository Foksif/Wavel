#pragma once
#include "../ipc/hyprland.hpp"
#include "module.hpp"
#include <nlohmann/json.hpp>

class KeyboardModule : public Module {
public:
  KeyboardModule(HyprlandIPC &ipc) : Module("keyboard"), ipc_(ipc) {
    query_devices();

    track(ipc_.signal_active_layout_changed().connect(
        [this]([[maybe_unused]] const std::string &keyboard,
               const std::string &layout) {
          active_layout_ = layout;
          rebuild();
        }));

    build();
  }

private:
  HyprlandIPC &ipc_;
  std::string active_layout_;

  Gtk::Label icon_;
  Gtk::Label layout_label_;

  void build() {
    icon_.set_text("󰗊");
    icon_.add_css_class("keyboard-icon");

    layout_label_.set_text(short_name(active_layout_));
    layout_label_.add_css_class("keyboard-layout");

    append(icon_);
    append(layout_label_);
  }

  void rebuild() { layout_label_.set_text(short_name(active_layout_)); }

  void query_devices() {
    try {
      auto devices = nlohmann::json::parse(ipc_.request("j/devices"));

      for (const auto &keyboard :
           devices.value("keyboards", nlohmann::json::array())) {
        if (keyboard.value("main", false)) {
          active_layout_ = keyboard.value("active_keymap", "");
          break;
        }
      }
    } catch (const nlohmann::json::exception &) {
      active_layout_.clear();
    }
  }

  static std::string short_name(const std::string &layout) {
    if (layout.rfind("English", 0) == 0)
      return "EN";
    if (layout.rfind("Russian", 0) == 0)
      return "RU";
    if (layout.rfind("Finnish", 0) == 0)
      return "FI";
    return layout.substr(0, 2);
  }
};
