#pragma once
#include "../ipc/hyprland.hpp"
#include "module.hpp"
#include <nlohmann/json.hpp>

class ActiveWindowModule : public Module {
public:
  explicit ActiveWindowModule(HyprlandIPC &ipc)
      : Module("activewindow"), ipc_(ipc) {
    label_.add_css_class("activewindow-label");
    label_.set_ellipsize(Pango::EllipsizeMode::END);
    label_.set_max_width_chars(40);
    append(label_);

    track(ipc_.signal_event().connect(
        [this](const std::string &event,
               [[maybe_unused]] const std::string &data) {
          if (event == "activewindow" || event == "windowtitlev2")
            refresh();
        }));

    track(ipc_.signal_tags_changed().connect([this] { refresh(); }));

    refresh();
  }

private:
  HyprlandIPC &ipc_;
  Gtk::Label label_;

  void refresh() {
    std::string text;
    try {
      std::string raw = ipc_.request("j/activewindow");
      auto pos = raw.find('{');
      if (pos != std::string::npos) {
        auto info = nlohmann::json::parse(raw.substr(pos));

        bool hidden = false;
        for (const auto &tag : info.value("tags", nlohmann::json::array())) {
          if (tag.is_string() &&
              tag.get<std::string>().rfind("noscreenshare", 0) == 0) {
            hidden = true;
            break;
          }
        }

        if (!hidden) {
          const std::string cls = info.value("class", "");
          const std::string title = info.value("title", "");
          if (!cls.empty() || !title.empty())
            text = cls + ": " + title;
        }
      }
    } catch (const nlohmann::json::exception &) {
      text.clear();
    }

    label_.set_text(text);
    set_visible(!text.empty());
  }
};
