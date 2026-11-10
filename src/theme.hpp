#pragma once

#include <gtkmm.h>
#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

struct ThemeInfo {
  std::string id;   // имя файла без .json
  std::string name; // отображаемое имя
  bool builtin;
};

class ThemeManager {
public:
  ThemeManager() = default;
  ~ThemeManager() { reload_timer_.disconnect(); }

  sigc::signal<void()> &signal_changed() { return changed_; }

  // Вызывать после того, как GTK инициализирован (в on_activate)
  void init() {
    config_dir_ = std::string(g_get_user_config_dir()) + "/wavel";
    themes_dir_ = config_dir_ + "/themes";
    config_file_ = config_dir_ + "/config.json";
    g_mkdir_with_parents(themes_dir_.c_str(), 0755);

    if (!fs::exists(config_file_))
      write_config(kFallback);

    base_css_ = load_resource_text("/io/github/foksif/wavel/wavel.css");

    provider_ = Gtk::CssProvider::create();
    provider_->signal_parsing_error().connect(
        [](const Glib::RefPtr<const Gtk::CssSection> &, const Glib::Error &e) {
          g_warning("CSS error: %s", e.what());
        });
    Gtk::StyleContext::add_provider_for_display(
        Gdk::Display::get_default(), provider_,
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);

    reload();
    watch(config_dir_);
    watch(themes_dir_);
  }

  // --- API для GUI ---
  std::vector<ThemeInfo> list() const {
    std::vector<ThemeInfo> v;
    for (auto &[id, e] : entries_)
      v.push_back(e.info);
    return v;
  }

  const std::string &current_id() const { return current_id_; }

  std::string current_name() const {
    auto it = entries_.find(current_id_);
    return it == entries_.end() ? current_id_ : it->second.info.name;
  }

  void set_theme(const std::string &id) {
    if (!entries_.count(id))
      return;
    write_config(id);
    reload();
  }

  void next_theme() {
    if (entries_.empty())
      return;
    auto it = entries_.find(current_id_);
    if (it != entries_.end())
      ++it;
    if (it == entries_.end())
      it = entries_.begin();
    set_theme(it->first);
  }

  // Для Cairo-рисования (остров)
  const GdkRGBA &rgba(const std::string &key) const {
    static const GdkRGBA none{0, 0, 0, 1};
    auto it = rgba_.find(key);
    return it == rgba_.end() ? none : it->second;
  }

private:
  using ColorMap = std::map<std::string, std::string>;

  struct Entry {
    ThemeInfo info;
    ColorMap colors; // только валидные переопределения
  };

  static constexpr const char *kFallback = "wavel";
  static constexpr const char *kResThemes = "/io/github/foksif/wavel/themes/";

  // Полный набор ключей. Всё, чего нет в теме, берётся отсюда
  static const ColorMap &defaults() {
    static const ColorMap d = {
        {"bg", "#1f1f28"},           // фон острова, модулей, попапов
        {"fg", "#f2d6dc"},           // основной текст
        {"fg_dim", "#a07a86"},       // приглушённый текст
        {"accent", "#f3a6bd"},       // главный акцент
        {"accent_hover", "#ff8fb3"}, // акцент при наведении
        {"secondary", "#d1a6c8"},    // вторичный акцент
        {"highlight", "#3a1f2a"},    // фон hover
        {"clock", "#e6b3a1"},        // часы
        {"good", "#7fbf9b"},         // батарея ок
        {"warn", "#ff8fb3"},         // батарея низкая
    };
    return d;
  }

  Glib::RefPtr<Gtk::CssProvider> provider_;
  std::vector<Glib::RefPtr<Gio::FileMonitor>> monitors_;
  sigc::connection reload_timer_;
  sigc::signal<void()> changed_;

  std::string config_dir_, themes_dir_, config_file_, base_css_;
  std::string current_id_, applied_sig_;
  std::map<std::string, Entry> entries_;
  std::map<std::string, GdkRGBA> rgba_;

  static std::string load_resource_text(const char *path) {
    GBytes *b =
        g_resources_lookup_data(path, G_RESOURCE_LOOKUP_FLAGS_NONE, nullptr);
    if (!b)
      return {};
    gsize n = 0;
    auto *d = static_cast<const char *>(g_bytes_get_data(b, &n));
    std::string s(d, n);
    g_bytes_unref(b);
    return s;
  }

  static std::string read_file(const std::string &path) {
    std::ifstream f(path);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
  }

  void add_entry(const std::string &text, const std::string &id, bool builtin) {
    try {
      auto j = nlohmann::json::parse(text);
      Entry e;
      e.info = {id, j.value("name", id), builtin};

      if (j.contains("colors") && j["colors"].is_object()) {
        for (auto &[k, v] : j["colors"].items()) {
          if (!defaults().count(k)) {
            g_warning("theme '%s': неизвестный ключ '%s'", id.c_str(),
                      k.c_str());
            continue;
          }
          GdkRGBA c;
          if (!v.is_string() ||
              !gdk_rgba_parse(&c, v.get<std::string>().c_str())) {
            g_warning("theme '%s': плохой цвет в '%s'", id.c_str(), k.c_str());
            continue;
          }
          e.colors[k] = v.get<std::string>();
        }
      }
      entries_[id] = std::move(e); // пользовательская перекрывает встроенную
    } catch (const std::exception &ex) {
      g_warning("theme '%s': %s", id.c_str(), ex.what());
    }
  }

  void scan() {
    entries_.clear();

    // встроенные
    GError *err = nullptr;
    char **names = g_resources_enumerate_children(
        kResThemes, G_RESOURCE_LOOKUP_FLAGS_NONE, &err);
    if (names) {
      for (char **n = names; *n; ++n) {
        fs::path p(*n);
        if (p.extension() != ".json")
          continue;
        add_entry(load_resource_text((std::string(kResThemes) + *n).c_str()),
                  p.stem().string(), true);
      }
      g_strfreev(names);
    } else {
      g_clear_error(&err);
    }

    // пользовательские
    std::error_code ec;
    fs::directory_iterator it(themes_dir_, ec);
    if (ec)
      return;
    for (auto &de : it) {
      if (!de.is_regular_file() || de.path().extension() != ".json")
        continue;
      add_entry(read_file(de.path().string()), de.path().stem().string(),
                false);
    }
  }

  std::string read_config_theme() const {
    try {
      auto j = nlohmann::json::parse(read_file(config_file_));
      return j.value("theme", std::string(kFallback));
    } catch (...) {
      return kFallback;
    }
  }

  void write_config(const std::string &id) {
    nlohmann::json j = nlohmann::json::object();
    try {
      std::ifstream f(config_file_);
      if (f)
        f >> j;
    } catch (...) {
      j = nlohmann::json::object();
    }
    if (!j.is_object())
      j = nlohmann::json::object();

    j["theme"] = id; // остальные ключи конфига сохраняются

    const std::string tmp = config_file_ + ".tmp";
    {
      std::ofstream o(tmp);
      o << j.dump(2) << "\n";
    }
    std::error_code ec;
    fs::rename(tmp, config_file_, ec);
  }

  void reload() {
    scan();

    std::string id = read_config_theme();
    auto it = entries_.find(id);
    if (it == entries_.end()) {
      g_warning("тема '%s' не найдена, использую '%s'", id.c_str(), kFallback);
      id = kFallback;
      it = entries_.find(id);
    }

    ColorMap eff = defaults();
    if (it != entries_.end())
      for (auto &[k, v] : it->second.colors)
        eff[k] = v;

    // Если ничего не изменилось (например, сработал монитор после нашей
    // же записи), то ничего не делаем
    std::string sig = id;
    for (auto &[k, v] : eff)
      sig += "|" + k + "=" + v;
    if (sig == applied_sig_)
      return;
    applied_sig_ = sig;
    current_id_ = id;

    apply(eff);
    changed_.emit();
  }

  void apply(const ColorMap &eff) {
    rgba_.clear();
    std::string css;

    for (auto &[k, v] : eff) {
      GdkRGBA c;
      gdk_rgba_parse(&c, v.c_str());
      rgba_[k] = c;

      char *s = gdk_rgba_to_string(&c);
      css += "@define-color wv_" + k + " " + s + ";\n";
      g_free(s);
    }
    css += base_css_;

    gtk_css_provider_load_from_string(provider_->gobj(), css.c_str());
  }

  void watch(const std::string &dir) {
    try {
      auto mon = Gio::File::create_for_path(dir)->monitor_directory();
      mon->signal_changed().connect(
          [this](const Glib::RefPtr<Gio::File> &,
                 const Glib::RefPtr<Gio::File> &,
                 Gio::FileMonitor::Event) { schedule_reload(); });
      monitors_.push_back(mon);
    } catch (const Glib::Error &e) {
      g_warning("не удалось следить за %s: %s", dir.c_str(), e.what());
    }
  }

  void schedule_reload() {
    reload_timer_.disconnect();
    reload_timer_ = Glib::signal_timeout().connect(
        [this] {
          reload();
          return false;
        },
        150);
  }
};
