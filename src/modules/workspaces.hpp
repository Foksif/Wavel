#pragma once
#include "../ipc/hyprland.hpp"
#include "module.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <map>
#include <string>
#include <vector>

class WorkspacesModule : public Module {
public:
  explicit WorkspacesModule(HyprlandIPC &ipc)
      : Module("workspaces"), ipc_(ipc) {
    track(ipc_.signal_workspace_changed().connect(
        [this](int id, const std::string &) { set_active(id); }));

    track(ipc_.signal_event().connect(
        [this](const std::string &ev,
               [[maybe_unused]] const std::string &data) {
          if (ev == "createworkspacev2" || ev == "destroyworkspacev2") {
            rebuild();
          } else if (ev == "focusedmon") {
            set_active(query_active_workspace());
          }
        }));
    rebuild();
  }

private:
  HyprlandIPC &ipc_;
  std::map<int, Gtk::Button *> buttons_;
  int active_ = -1;

  void rebuild() {
    std::vector<int> ids;
    try {
      auto j = nlohmann::json::parse(ipc_.request("j/workspaces"));
      for (const auto &ws : j) {
        int id = ws.value("id", 0);
        if (id > 0)
          ids.push_back(id);
      }
    } catch (const nlohmann::json::exception &e) {
      g_warning("Workspaces: ошибка JSON: %s", e.what());
      return;
    }
    std::sort(ids.begin(), ids.end());

    for (auto it = buttons_.begin(); it != buttons_.end();) {
      if (std::find(ids.begin(), ids.end(), it->first) == ids.end()) {
        remove(*it->second);
        it = buttons_.erase(it);
      } else {
        ++it;
      }
    }

    for (int id : ids)
      if (!buttons_.count(id))
        buttons_[id] = make_button(id);

    Gtk::Widget *prev = nullptr;

    for (auto &[id, btn] : buttons_) {
      if (prev)
        reorder_child_after(*btn, *prev);

      prev = btn;
    }
    set_active(query_active_workspace());
  }

  Gtk::Button *make_button(int id) {
    auto *btn = Gtk::make_managed<Gtk::Button>(std::to_string(id));
    btn->add_css_class("workspace");
    btn->set_focus_on_click(false);
    btn->signal_clicked().connect([this, id] { ipc_.dispatch_workspace(id); });
    append(*btn);
    return btn;
  }

  void set_active(int id) {
    active_ = id;
    for (auto &[wid, btn] : buttons_) {
      if (wid == id)
        btn->add_css_class("active");
      else
        btn->remove_css_class("active");
    }
  }

  int query_active_workspace() {
    try {
      return nlohmann::json::parse(ipc_.request("j/activeworkspace"))
          .value("id", -1);
    } catch (const nlohmann::json::exception &) {
      return -1;
    }
  }
};
