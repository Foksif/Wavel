#pragma once

#include "island/mpris.hpp"
#include "island/page.hpp"

#include <gio/gio.h>
#include <glibmm/spawn.h>
#include <gtkmm.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>

class HomePage : public IslandPage {
public:
  explicit HomePage(MprisClient &mpris)
      : IslandPage("home", "\U000F02DC", "Главная"), mpris_(mpris) {
    auto *root = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 16);
    root->set_vexpand(true);
    root->set_valign(Gtk::Align::CENTER);
    root->set_homogeneous(true);

    root->append(*build_left());
    root->append(*build_stats());
    append(*root);

    find_gpu();

    mpris_.signal_changed().connect(
        sigc::mem_fun(*this, &HomePage::update_media));
    timer_ = Glib::signal_timeout().connect_seconds(
        [this] {
          update_clock();
          return true;
        },
        1);

    signal_map().connect([this] { start_stats(); });
    signal_unmap().connect([this] { stop_stats(); });

    update_clock();
    update_media();
  }

  ~HomePage() override {
    *alive_ = false;
    timer_.disconnect();
    stats_timer_.disconnect();
  }

  void on_shown() override {
    update_clock();
    update_media();
  }

private:
  struct Stat {
    Gtk::Box *root = nullptr;
    Gtk::Label *value = nullptr;
    Gtk::ProgressBar *bar = nullptr;
  };

  Gtk::Widget *build_left() {
    auto *col = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 2);
    col->set_valign(Gtk::Align::CENTER);
    col->set_halign(Gtk::Align::START);
    col->set_margin_start(6);

    clock_.add_css_class("island-home-clock");
    date_.add_css_class("island-home-date");
    media_.add_css_class("island-home-media");
    for (auto *l : {&clock_, &date_, &media_})
      l->set_halign(Gtk::Align::START);
    media_.set_ellipsize(Pango::EllipsizeMode::END);
    media_.set_max_width_chars(24);
    media_.set_margin_top(6);

    col->append(clock_);
    col->append(date_);
    col->append(media_);
    col->append(*build_actions());
    return col;
  }

  Gtk::Widget *build_actions() {
    auto *row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 6);
    row->set_margin_top(12);

    struct Action {
      const char *glyph, *tip, *cmd;
      bool danger;
    };
    static constexpr Action kActions[] = {
        {"\U000F033E", "Заблокировать", "hyprlock", false},
        //        {"\U000F0100", "Скриншот области", "@screenshot", false},
        {"\U000F0425", "Выключить", "systemctl poweroff", true},
    };

    for (auto &a : kActions) {
      auto *icon = Gtk::make_managed<Gtk::Label>(a.glyph);
      icon->add_css_class("island-action-icon");
      auto *b = Gtk::make_managed<Gtk::Button>();
      b->set_child(*icon);
      b->set_has_frame(false);
      b->add_css_class("island-action");
      if (a.danger)
        b->add_css_class("danger");
      b->set_tooltip_text(a.tip);
      b->signal_clicked().connect([cmd = std::string(a.cmd)] {
        if (cmd == "@screenshot") {
          screenshot_area();
          return;
        }
        try {
          Glib::spawn_command_line_async(cmd);
        } catch (const Glib::Error &e) {
          g_warning("action '%s': %s", cmd.c_str(), e.what());
        }
      });
      row->append(*b);
    }
    return row;
  }

  static void screenshot_area() {
    const char *argv[] = {"slurp", nullptr};
    GError *err = nullptr;
    GSubprocess *p =
        g_subprocess_newv(argv, G_SUBPROCESS_FLAGS_STDOUT_PIPE, &err);
    if (!p) {
      g_warning("slurp: %s", err->message);
      g_clear_error(&err);
      return;
    }
    g_subprocess_communicate_utf8_async(p, nullptr, nullptr, shot_on_slurp,
                                        nullptr);
  }

  static void shot_on_slurp(GObject *src, GAsyncResult *res, gpointer) {
    GSubprocess *p = G_SUBPROCESS(src);
    char *out = nullptr;
    GError *err = nullptr;
    gboolean ok =
        g_subprocess_communicate_utf8_finish(p, res, &out, nullptr, &err);
    if (!ok) {
      g_warning("slurp: %s", err->message);
      g_clear_error(&err);
    } else if (out && *g_strstrip(out)) {
      const char *argv[] = {"grim", "-g", out, "-", nullptr};
      GSubprocess *g =
          g_subprocess_newv(argv, G_SUBPROCESS_FLAGS_STDOUT_PIPE, &err);
      if (!g) {
        g_warning("grim: %s", err->message);
        g_clear_error(&err);
      } else {
        g_subprocess_communicate_async(g, nullptr, nullptr, shot_on_grim,
                                       nullptr);
      }
    }
    g_free(out);
    g_object_unref(p);
  }

  static void shot_on_grim(GObject *src, GAsyncResult *res, gpointer) {
    GSubprocess *g = G_SUBPROCESS(src);
    GBytes *png = nullptr;
    GError *err = nullptr;
    gboolean ok = g_subprocess_communicate_finish(g, res, &png, nullptr, &err);
    if (!ok || !png || g_bytes_get_size(png) == 0) {
      g_warning("grim: %s", err ? err->message : "empty output");
      g_clear_error(&err);
    } else {
      const char *argv[] = {"wl-copy", "-t", "image/png", nullptr};
      GSubprocess *w = g_subprocess_newv(
          argv,
          static_cast<GSubprocessFlags>(G_SUBPROCESS_FLAGS_STDIN_PIPE |
                                        G_SUBPROCESS_FLAGS_STDOUT_SILENCE |
                                        G_SUBPROCESS_FLAGS_STDERR_SILENCE),
          &err);
      if (!w) {
        g_warning("wl-copy: %s", err->message);
        g_clear_error(&err);
      } else {
        g_subprocess_communicate_async(w, png, nullptr, shot_on_copy, nullptr);
      }
    }
    if (png)
      g_bytes_unref(png);
    g_object_unref(g);
  }

  static void shot_on_copy(GObject *src, GAsyncResult *res, gpointer) {
    GSubprocess *w = G_SUBPROCESS(src);
    GError *err = nullptr;
    if (!g_subprocess_communicate_finish(w, res, nullptr, nullptr, &err)) {
      g_warning("wl-copy: %s", err->message);
      g_clear_error(&err);
    }
    g_object_unref(w);
  }

  Stat make_stat(const char *glyph, const char *name) {
    Stat s;
    s.root = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 10);

    auto *icon = Gtk::make_managed<Gtk::Label>(glyph);
    icon->add_css_class("island-stat-icon");
    icon->set_valign(Gtk::Align::CENTER);

    auto *body = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 3);
    body->set_hexpand(true);

    auto *head = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 0);
    auto *title = Gtk::make_managed<Gtk::Label>(name);
    title->add_css_class("island-stat-name");
    title->set_halign(Gtk::Align::START);
    title->set_hexpand(true);

    s.value = Gtk::make_managed<Gtk::Label>("--");
    s.value->add_css_class("island-stat-value");
    s.value->set_halign(Gtk::Align::END);

    head->append(*title);
    head->append(*s.value);

    s.bar = Gtk::make_managed<Gtk::ProgressBar>();
    s.bar->add_css_class("island-stat-bar");

    body->append(*head);
    body->append(*s.bar);
    s.root->append(*icon);
    s.root->append(*body);
    return s;
  }

  Gtk::Widget *build_stats() {
    auto *card = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 12);
    card->add_css_class("island-stats");
    card->set_valign(Gtk::Align::CENTER);

    cpu_ = make_stat("\U000F061A", "CPU");
    gpu_ = make_stat("\U000F08AE", "GPU");
    ram_ = make_stat("\U000F035B", "RAM");

    card->append(*cpu_.root);
    card->append(*gpu_.root);
    card->append(*ram_.root);
    gpu_.root->set_visible(false);
    return card;
  }

  static void set_stat(Stat &s, double frac, const std::string &text) {
    frac = std::clamp(frac, 0.0, 1.0);
    s.bar->set_fraction(frac);
    s.value->set_text(text);
    if (frac > 0.85)
      s.bar->add_css_class("hot");
    else
      s.bar->remove_css_class("hot");
  }

  static std::string pct(double f) {
    return std::to_string(static_cast<int>(f * 100.0 + 0.5)) + "%";
  }

  void start_stats() {
    if (stats_timer_.connected())
      return;
    prev_total_ = prev_idle_ = 0;
    update_stats();
    stats_timer_ = Glib::signal_timeout().connect(
        [this] {
          update_stats();
          return true;
        },
        1000);
  }

  void stop_stats() { stats_timer_.disconnect(); }

  void update_stats() {
    update_cpu();
    update_ram();
    update_gpu();
  }

  void update_cpu() {
    FILE *f = fopen("/proc/stat", "r");
    if (!f)
      return;
    unsigned long long v[8] = {};
    int n = fscanf(f, "cpu %llu %llu %llu %llu %llu %llu %llu %llu", &v[0],
                   &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7]);
    fclose(f);
    if (n != 8)
      return;

    unsigned long long idle = v[3] + v[4], total = 0;
    for (auto x : v)
      total += x;

    if (prev_total_ != 0 && total > prev_total_) {
      const double busy =
          1.0 - static_cast<double>(idle - prev_idle_) / (total - prev_total_);
      set_stat(cpu_, busy, pct(busy));
    }
    prev_total_ = total;
    prev_idle_ = idle;
  }

  void update_ram() {
    FILE *f = fopen("/proc/meminfo", "r");
    if (!f)
      return;
    char line[128];
    unsigned long long val = 0, total = 0, avail = 0;
    while (fgets(line, sizeof line, f)) {
      if (sscanf(line, "MemTotal: %llu", &val) == 1)
        total = val;
      else if (sscanf(line, "MemAvailable: %llu", &val) == 1)
        avail = val;
      if (total && avail)
        break;
    }
    fclose(f);
    if (!total)
      return;

    const double used = static_cast<double>(total - avail);
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.1f GB", used / 1048576.0);
    set_stat(ram_, used / total, buf);
  }

  void find_gpu() {
    namespace fs = std::filesystem;
    std::error_code ec;

    for (auto &de : fs::directory_iterator("/sys/class/drm", ec)) {
      auto p = de.path() / "device" / "gpu_busy_percent";
      if (fs::exists(p, ec)) {
        gpu_sysfs_ = p.string();
        gpu_.root->set_visible(true);
        return;
      }
    }

    if (gchar *p = g_find_program_in_path("nvidia-smi")) {
      g_free(p);
      gpu_nvidia_ = true;
      gpu_.root->set_visible(true);
    }
  }

  void update_gpu() {
    if (!gpu_sysfs_.empty()) {
      FILE *f = fopen(gpu_sysfs_.c_str(), "r");
      if (!f)
        return;
      int v = -1;
      int n = fscanf(f, "%d", &v);
      fclose(f);
      if (n == 1)
        set_stat(gpu_, v / 100.0, std::to_string(v) + "%");
      return;
    }
    if (gpu_nvidia_ && !gpu_busy_)
      query_nvidia();
  }

  struct GpuJob {
    HomePage *self;
    std::shared_ptr<bool> alive;
  };

  void query_nvidia() {
    const char *argv[] = {"nvidia-smi", "--query-gpu=utilization.gpu",
                          "--format=csv,noheader,nounits", nullptr};
    GError *err = nullptr;
    GSubprocess *p =
        g_subprocess_newv(argv, G_SUBPROCESS_FLAGS_STDOUT_PIPE, &err);
    if (!p) {
      g_clear_error(&err);
      gpu_nvidia_ = false;
      gpu_.root->set_visible(false);
      return;
    }
    gpu_busy_ = true;

    auto *job = new GpuJob{this, alive_};
    g_subprocess_communicate_utf8_async(
        p, nullptr, nullptr,
        +[](GObject *src, GAsyncResult *res, gpointer data) {
          std::unique_ptr<GpuJob> job(static_cast<GpuJob *>(data));
          GSubprocess *p = G_SUBPROCESS(src);

          char *out = nullptr;
          GError *e = nullptr;
          const bool ok =
              g_subprocess_communicate_utf8_finish(p, res, &out, nullptr, &e);
          g_clear_error(&e);

          if (*job->alive) {
            job->self->gpu_busy_ = false;
            if (ok && out) {
              const int v = std::atoi(out);
              set_stat(job->self->gpu_, v / 100.0, std::to_string(v) + "%");
            }
          }
          g_free(out);
          g_object_unref(p);
        },
        job);
  }

  void update_clock() {
    auto now = Glib::DateTime::create_now_local();
    clock_.set_text(now.format("%H:%M"));
    date_.set_text(now.format("%A, %d %B"));
  }

  void update_media() {
    const MediaInfo *m = mpris_.current();
    if (!m || !m->playing) {
      media_.set_visible(false);
      return;
    }
    media_.set_text(m->artist.empty() ? m->title
                                      : m->title + " — " + m->artist);
    media_.set_visible(true);
  }

  MprisClient &mpris_;
  Gtk::Label clock_, date_, media_;
  Stat cpu_, gpu_, ram_;
  sigc::connection timer_, stats_timer_;

  unsigned long long prev_total_ = 0, prev_idle_ = 0;
  std::string gpu_sysfs_;
  bool gpu_nvidia_ = false;
  bool gpu_busy_ = false;
  std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
};
