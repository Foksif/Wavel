#pragma once

#include <gtkmm.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

struct ModuleInfo {
  const char *id;
  const char *title;
};

inline void reparent(Gtk::Widget &w, Gtk::Box &dst) {
  g_object_ref(w.gobj());
  if (auto *old = dynamic_cast<Gtk::Box *>(w.get_parent()))
    old->remove(w);
  dst.append(w);
  g_object_unref(w.gobj());
}

class LayoutManager {
public:
  enum Column { Left = 0, Hidden = 1, Right = 2 };

  static constexpr ModuleInfo kModules[] = {
      {"workspaces", "Рабочие столы"},
      {"activewindow", "Окно"},
      {"audio", "Звук"},
      {"brightness", "Яркость"},
      {"keyboard", "Раскладка"},
      {"battery", "Батарея"},
      {"tray", "Трей"},
  };

  ~LayoutManager() { timer_.disconnect(); }

  sigc::signal<void()> &signal_changed() { return changed_; }

  void init() {
    const std::string dir = std::string(g_get_user_config_dir()) + "/wavel";
    file_ = dir + "/config.json";
    load();

    try {
      monitor_ = Gio::File::create_for_path(dir)->monitor_directory();
      monitor_->signal_changed().connect([this](const Glib::RefPtr<Gio::File> &,
                                                const Glib::RefPtr<Gio::File> &,
                                                Gio::FileMonitor::Event) {
        timer_.disconnect();
        timer_ = Glib::signal_timeout().connect(
            [this] {
              if (load())
                changed_.emit();
              return false;
            },
            150);
      });
    } catch (const Glib::Error &e) {
      g_warning("layout: не удалось следить за конфигом: %s", e.what());
    }
  }

  const std::vector<std::string> &left() const { return left_; }
  const std::vector<std::string> &right() const { return right_; }

  std::vector<std::string> hidden() const {
    std::vector<std::string> v;
    for (auto &m : kModules)
      if (column_of(m.id) == Hidden)
        v.push_back(m.id);
    return v;
  }

  Column column_of(const std::string &id) const {
    if (has(left_, id))
      return Left;
    if (has(right_, id))
      return Right;
    return Hidden;
  }

  void move(const std::string &id, int dir) {
    const int from = column_of(id);
    const int to = std::clamp(from + dir, 0, 2);
    if (from == to)
      return;

    erase(left_, id);
    erase(right_, id);
    if (to == Left)
      left_.push_back(id);
    if (to == Right)
      right_.push_back(id);
    save();
  }

  void bump(const std::string &id) {
    auto &v = column_of(id) == Left ? left_ : right_;
    auto it = std::find(v.begin(), v.end(), id);
    if (v.size() < 2 || it == v.end())
      return;
    if (it == v.begin())
      std::rotate(v.begin(), v.begin() + 1, v.end());
    else
      std::iter_swap(it, it - 1);
    save();
  }

private:
  static bool has(const std::vector<std::string> &v, const std::string &id) {
    return std::find(v.begin(), v.end(), id) != v.end();
  }
  static void erase(std::vector<std::string> &v, const std::string &id) {
    v.erase(std::remove(v.begin(), v.end(), id), v.end());
  }
  static bool known(const std::string &id) {
    for (auto &m : kModules)
      if (id == m.id)
        return true;
    return false;
  }

  bool load() {
    std::vector<std::string> l = {"workspaces", "activewindow"};
    std::vector<std::string> r = {"audio", "keyboard", "battery", "tray"};

    try {
      std::ifstream f(file_);
      std::stringstream ss;
      ss << f.rdbuf();
      auto j = nlohmann::json::parse(ss.str());

      if (j.contains("layout") && j["layout"].is_object()) {
        l.clear();
        r.clear();
        std::set<std::string> seen;
        auto take = [&](const char *key, std::vector<std::string> &out) {
          auto it = j["layout"].find(key);
          if (it == j["layout"].end() || !it->is_array())
            return;
          for (auto &v : *it) {
            if (!v.is_string())
              continue;
            auto id = v.get<std::string>();
            if (known(id) && seen.insert(id).second)
              out.push_back(id);
          }
        };
        take("left", l);
        take("right", r);
      }
    } catch (...) {
      return false;
    }

    if (l == left_ && r == right_)
      return false;
    left_ = std::move(l);
    right_ = std::move(r);
    return true;
  }

  void save() {
    nlohmann::json j = nlohmann::json::object();
    try {
      std::ifstream f(file_);
      if (f)
        f >> j;
    } catch (...) {
      j = nlohmann::json::object();
    }
    if (!j.is_object())
      j = nlohmann::json::object();

    j["layout"] = {{"left", left_}, {"right", right_}};

    const std::string tmp = file_ + ".tmp";
    {
      std::ofstream o(tmp);
      o << j.dump(2) << "\n";
    }
    std::error_code ec;
    std::filesystem::rename(tmp, file_, ec);

    changed_.emit();
  }

  std::vector<std::string> left_ = {"workspaces", "activewindow"};
  std::vector<std::string> right_ = {"audio", "keyboard", "battery", "tray"};
  std::string file_;
  Glib::RefPtr<Gio::FileMonitor> monitor_;
  sigc::connection timer_;
  sigc::signal<void()> changed_;
};
