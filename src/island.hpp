#pragma once

#include "island/expanded.hpp"
#include "island/mpris.hpp"
#include "island/notifications.hpp"
#include "island/pages/home.hpp"
#include "island/pages/themes.hpp"
#include "island/spring.hpp"
#include "island/state.hpp"
#include "theme.hpp"

#include <glib/gstdio.h>
#include <gtk4-layer-shell.h>
#include <gtkmm.h>

#include <algorithm>
#include <cmath>
#include <functional>
#include <memory>
#include <string>

class IslandWindow : public Gtk::Window {
public:
  IslandWindow(GdkMonitor *monitor, MprisClient &mpris,
               NotificationServer &notifications, ThemeManager &themes)
      : mpris_(mpris), themes_(themes) {
    add_css_class("wavel-island");
    set_decorated(false);
    set_default_size(island::kMaxWidth + 2 * static_cast<int>(island::kFlare),
                     island::kMaxHeight);

    setup_layer_shell(monitor);
    setup_content();
    setup_input();

    state_ = base_state();
    const auto size = island::size_for(state_);
    width_ = {size.w, 0.0f, size.w};
    height_ = {size.h, 0.0f, size.h};

    content_.set_visible_child(page_name(state_));
    refresh_media();
    update_clock();
    update_content();

    themes_.signal_changed().connect(
        sigc::mem_fun(*this, &IslandWindow::on_theme_changed));

    mpris_.signal_changed().connect(
        sigc::mem_fun(*this, &IslandWindow::on_media_changed));
    notifications.signal_notification().connect(
        sigc::mem_fun(*this, &IslandWindow::on_notification));

    clock_timer_ = Glib::signal_timeout().connect_seconds(
        [this] {
          update_clock();
          return true;
        },
        1);

    signal_map().connect([this] { apply_input_region(); });
  }

  ~IslandWindow() override {
    *alive_ = false;
    leave_timer_.disconnect();
    notif_timer_.disconnect();
    clock_timer_.disconnect();
  }

private:
  enum class MusicFx {
    Off,   // ничего
    Waves, // плавные волны
    Peaks  // острые пики
  };

  static constexpr MusicFx kMusicFx = MusicFx::Peaks;

  static constexpr bool kJelly = true;         // желе при открытии/закрытии
  static constexpr float kWobbleGain = 0.016f; // сила желе
  static constexpr float kWobbleMax = 17.0f;   // макс. изгиб края, px

  static constexpr float kWaveAmp = 6.0f;    // Waves: высота волны, px
  static constexpr float kPeakAmp = 6.0f;    // Peaks: высота пиков, px
  static constexpr float kPeakWidth = 18.0f; // Peaks: ширина одного зубца, px
  static constexpr float kPeakSpeed = 1.0f;  // Peaks: скорость

  static constexpr bool kExpandedAutoClose = true;

  static constexpr int kSmallCover = 22;
  static constexpr int kBigCover = 112;

  static constexpr const char *kPrev = "\U000F04AE";
  static constexpr const char *kNext = "\U000F04AD";
  static constexpr const char *kPlay = "\U000F040A";
  static constexpr const char *kPause = "\U000F03E4";
  static constexpr const char *kExpand = "\U000F0140"; // chevron-down

  void setup_layer_shell(GdkMonitor *monitor) {
    GtkWindow *gw = GTK_WINDOW(gobj());

    gtk_layer_init_for_window(gw);
    gtk_layer_set_namespace(gw, "wavel-island");
    gtk_layer_set_monitor(gw, monitor);
    gtk_layer_set_layer(gw, GTK_LAYER_SHELL_LAYER_TOP);
    gtk_layer_set_anchor(gw, GTK_LAYER_SHELL_EDGE_TOP, true);

    gtk_layer_set_exclusive_zone(gw, -1);
    gtk_layer_set_keyboard_mode(gw, GTK_LAYER_SHELL_KEYBOARD_MODE_NONE);
  }

  static const char *page_name(island::State s) {
    switch (s) {
    case island::State::Idle:
      return "idle";
    case island::State::Music:
      return "music";
    case island::State::Notification:
      return "notif";
    case island::State::Full:
      return "full";
    case island::State::Expanded:
      return "expanded";
    }
    return "idle";
  }

  Gtk::Label *make_label(const char *text, const char *css) {
    auto *l = Gtk::make_managed<Gtk::Label>(text);
    l->add_css_class(css);
    return l;
  }

  Gtk::Picture *make_cover(int size, const char *extra_css = nullptr) {
    auto *p = Gtk::make_managed<Gtk::Picture>();
    gtk_picture_set_content_fit(p->gobj(), GTK_CONTENT_FIT_COVER);
    p->set_can_shrink(true);
    p->set_size_request(size, size);
    p->set_halign(Gtk::Align::CENTER);
    p->set_valign(Gtk::Align::CENTER);
    p->add_css_class("island-cover");
    if (extra_css)
      p->add_css_class(extra_css);
    return p;
  }

  Gtk::Box *frame(Gtk::Widget &inner, island::State s) {
    const auto size = island::size_for(s);
    auto *f = Gtk::make_managed<Gtk::Box>();
    f->set_size_request(static_cast<int>(size.w), static_cast<int>(size.h));
    f->set_halign(Gtk::Align::CENTER);
    f->set_valign(Gtk::Align::START);
    inner.set_hexpand(true);
    inner.set_vexpand(true);
    f->append(inner);
    return f;
  }

  Gtk::Button *make_button(const char *glyph, std::function<void()> cb,
                           bool main = false,
                           Gtk::Label **label_out = nullptr) {
    auto *label = Gtk::make_managed<Gtk::Label>(glyph);
    label->add_css_class("island-btn-icon");

    auto *b = Gtk::make_managed<Gtk::Button>();
    b->set_child(*label);
    b->set_has_frame(false);
    b->add_css_class("island-btn");
    if (main)
      b->add_css_class("island-btn-main");
    b->signal_clicked().connect(std::move(cb));

    if (label_out)
      *label_out = label;
    return b;
  }

  void build_idle_page() {
    auto *inner = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 0);
    clock_label_ = make_label("", "island-clock");
    clock_label_->set_hexpand(true);
    clock_label_->set_halign(Gtk::Align::CENTER);
    clock_label_->set_valign(Gtk::Align::CENTER);
    inner->append(*clock_label_);
    content_.add(*frame(*inner, island::State::Idle), "idle");
  }

  void build_music_page() {
    auto *inner = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
    inner->set_margin_start(14);
    inner->set_margin_end(14);

    cover_small_ = make_cover(kSmallCover);

    compact_label_ = make_label("", "island-compact-text");
    compact_label_->set_halign(Gtk::Align::START);
    compact_label_->set_valign(Gtk::Align::CENTER);
    compact_label_->set_ellipsize(Pango::EllipsizeMode::END);
    compact_label_->set_max_width_chars(26);

    inner->append(*cover_small_);
    inner->append(*compact_label_);
    content_.add(*frame(*inner, island::State::Music), "music");

    inner->set_hexpand(false);
    inner->set_vexpand(false);
    inner->set_halign(Gtk::Align::CENTER);
    inner->set_valign(Gtk::Align::CENTER);
  }

  void build_notification_page() {
    auto *inner = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 12);
    inner->set_margin_start(20);
    inner->set_margin_end(20);
    inner->set_margin_top(8);
    inner->set_margin_bottom(8);

    notif_icon_ = Gtk::make_managed<Gtk::Image>();
    notif_icon_->set_pixel_size(32);
    notif_icon_->set_valign(Gtk::Align::CENTER);
    notif_icon_->add_css_class("island-notif-icon");

    auto *text = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 2);
    text->set_valign(Gtk::Align::CENTER);
    text->set_hexpand(true);

    notif_title_ = make_label("", "island-notif-title");
    notif_title_->set_halign(Gtk::Align::START);
    notif_title_->set_ellipsize(Pango::EllipsizeMode::END);
    notif_title_->set_max_width_chars(36);

    notif_body_ = make_label("", "island-notif-body");
    notif_body_->set_halign(Gtk::Align::START);
    notif_body_->set_ellipsize(Pango::EllipsizeMode::END);
    notif_body_->set_max_width_chars(40);

    text->append(*notif_title_);
    text->append(*notif_body_);
    inner->append(*notif_icon_);
    inner->append(*text);
    content_.add(*frame(*inner, island::State::Notification), "notif");
  }

  void build_full_page() {
    auto *inner = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 18);
    inner->set_margin_start(22);
    inner->set_margin_end(22);
    inner->set_margin_top(22);
    inner->set_margin_bottom(22);

    cover_big_ = make_cover(kBigCover, "island-cover-big");

    auto *col = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 2);
    col->set_valign(Gtk::Align::CENTER);
    col->set_hexpand(true);

    full_title_ = make_label("", "island-title");
    full_title_->set_halign(Gtk::Align::START);
    full_title_->set_ellipsize(Pango::EllipsizeMode::END);
    full_title_->set_max_width_chars(20);

    full_artist_ = make_label("", "island-artist");
    full_artist_->set_halign(Gtk::Align::START);
    full_artist_->set_ellipsize(Pango::EllipsizeMode::END);
    full_artist_->set_max_width_chars(24);

    auto *controls =
        Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 6);
    controls->set_margin_top(12);
    controls->set_halign(Gtk::Align::START);
    controls->append(*make_button(kPrev, [this] { mpris_.previous(); }));
    controls->append(*make_button(
        kPlay, [this] { mpris_.play_pause(); }, true, &play_icon_));
    controls->append(*make_button(kNext, [this] { mpris_.next(); }));

    auto *expand =
        make_button(kExpand, [this] { set_state(island::State::Expanded); });
    expand->set_margin_start(10);
    expand->set_tooltip_text("Развернуть");
    controls->append(*expand);

    col->append(*full_title_);
    col->append(*full_artist_);
    col->append(*controls);

    inner->append(*cover_big_);
    inner->append(*col);
    content_.add(*frame(*inner, island::State::Full), "full");
  }

  // Регистрация Expanded страниц
  void build_expanded_page() {
    expanded_ =
        Gtk::make_managed<ExpandedView>([this] { set_state(base_state()); });

    expanded_->add_page(*Gtk::make_managed<HomePage>(mpris_));
    expanded_->add_page(*Gtk::make_managed<ThemesPage>(themes_));

    content_.add(*frame(*expanded_, island::State::Expanded), "expanded");
  }

  void setup_content() {
    area_.set_hexpand(true);
    area_.set_vexpand(true);
    area_.set_draw_func(sigc::mem_fun(*this, &IslandWindow::on_draw));

    content_.set_transition_type(Gtk::StackTransitionType::CROSSFADE);
    content_.set_transition_duration(180);

    build_idle_page();
    build_music_page();
    build_notification_page();
    build_full_page();
    build_expanded_page();

    clip_.set_policy(Gtk::PolicyType::EXTERNAL, Gtk::PolicyType::EXTERNAL);
    clip_.set_kinetic_scrolling(false);
    clip_.set_halign(Gtk::Align::CENTER);
    clip_.set_valign(Gtk::Align::START);
    clip_.set_child(content_);

    // Хз полчему но без этого он сходит с сума
    for (auto adj : {clip_.get_vadjustment(), clip_.get_hadjustment()}) {
      adj->signal_value_changed().connect([adj] {
        if (adj->get_value() != 0.0)
          adj->set_value(0.0);
      });
    }

    overlay_.set_child(area_);
    overlay_.add_overlay(clip_);
    set_child(overlay_);
  }

  void setup_input() {
    auto motion = Gtk::EventControllerMotion::create();
    motion->signal_enter().connect(
        [this](double, double) { on_pointer_enter(); });
    motion->signal_leave().connect([this] { on_pointer_leave(); });
    overlay_.add_controller(motion);

    auto click = Gtk::GestureClick::create();
    click->signal_pressed().connect(
        [this](int, double, double) { on_click(); });
    overlay_.add_controller(click);
  }

  bool auto_closable() const {
    return state_ == island::State::Full ||
           (kExpandedAutoClose && state_ == island::State::Expanded);
  }

  void on_pointer_enter() {
    hovered_ = true;
    leave_timer_.disconnect();
  }

  void on_pointer_leave() {
    hovered_ = false;
    leave_timer_.disconnect();

    if (!auto_closable())
      return;

    leave_timer_ = Glib::signal_timeout().connect(
        [this] {
          if (!hovered_ && auto_closable())
            set_state(base_state());
          return false;
        },
        500);
  }

  void on_click() {
    notif_timer_.disconnect();

    switch (state_) {
    case island::State::Idle:
      set_state(island::State::Expanded);
      break;
    case island::State::Music:
      set_state(island::State::Full);
      break;
    case island::State::Full:
    case island::State::Notification:
      set_state(base_state());
      break;
    case island::State::Expanded:
      break;
    }
  }

  island::State base_state() const {
    const MediaInfo *m = mpris_.current();
    return (m && m->playing) ? island::State::Music : island::State::Idle;
  }

  void set_state(island::State s) {
    state_ = s;

    const auto size = island::size_for(s);
    width_.target = size.w;
    height_.target = size.h;

    if (s != island::State::Notification)
      notif_timer_.disconnect();

    if (s == island::State::Expanded)
      expanded_->refresh();

    content_.set_visible_child(page_name(s));
    start_animation();
  }

  void on_media_changed() {
    refresh_media();
    if (state_ == island::State::Idle || state_ == island::State::Music)
      set_state(base_state());
  }

  void on_theme_changed() { area_.queue_draw(); }

  void on_notification(const Notification &n) {
    if (state_ == island::State::Full || state_ == island::State::Expanded)
      return;

    if (!n.icon.empty() && n.icon[0] == '/')
      gtk_image_set_from_file(notif_icon_->gobj(), n.icon.c_str());
    else
      notif_icon_->set_from_icon_name(
          n.icon.empty() ? "dialog-information-symbolic" : n.icon);

    notif_title_->set_text(n.summary.empty() ? n.app : n.summary);
    notif_body_->set_text(n.body);
    notif_body_->set_visible(!n.body.empty());

    set_state(island::State::Notification);

    notif_timer_.disconnect();
    notif_timer_ = Glib::signal_timeout().connect(
        [this] {
          if (state_ == island::State::Notification)
            set_state(base_state());
          return false;
        },
        4500);
  }

  void update_clock() {
    clock_label_->set_text(Glib::DateTime::create_now_local().format("%H:%M"));
  }

  void refresh_media() {
    const MediaInfo *m = mpris_.current();

    std::string title = "Nothing playing";
    std::string artist;
    std::string art;
    bool playing = false;

    if (m) {
      title = m->title.empty() ? "Unknown" : m->title;
      artist = m->artist;
      art = m->art_url;
      playing = m->playing;
    }

    compact_label_->set_text(artist.empty() ? title : title + " — " + artist);
    full_title_->set_text(title);
    full_artist_->set_text(artist);
    play_icon_->set_text(playing ? kPause : kPlay);

    load_cover(art);
  }

  static void apply_cover(Gtk::Picture *pic, const std::string &path,
                          int size) {
    if (path.empty()) {
      gtk_picture_set_paintable(pic->gobj(), nullptr);
      return;
    }
    GError *err = nullptr;
    GdkPixbuf *pb = gdk_pixbuf_new_from_file_at_scale(path.c_str(), size, size,
                                                      FALSE, &err);
    if (!pb) {
      g_clear_error(&err);
      gtk_picture_set_paintable(pic->gobj(), nullptr);
      return;
    }
    GdkTexture *tex = gdk_texture_new_for_pixbuf(pb);
    gtk_picture_set_paintable(pic->gobj(), GDK_PAINTABLE(tex));
    g_object_unref(tex);
    g_object_unref(pb);
  }

  void set_cover_file(const std::string &path) {
    apply_cover(cover_small_, path, kSmallCover);
    apply_cover(cover_big_, path, kBigCover);
  }

  struct CoverJob {
    IslandWindow *self;
    std::shared_ptr<bool> alive;
    std::string url, path, tmp;
  };

  void load_cover(const std::string &url) {
    if (url == cover_url_)
      return;
    cover_url_ = url;
    set_cover_file("");

    if (url.empty())
      return;

    if (g_str_has_prefix(url.c_str(), "file://")) {
      gchar *p = g_filename_from_uri(url.c_str(), nullptr, nullptr);
      if (p) {
        set_cover_file(p);
        g_free(p);
      }
      return;
    }

    if (!g_str_has_prefix(url.c_str(), "http"))
      return;

    const std::string dir = std::string(g_get_user_cache_dir()) + "/wavel";
    g_mkdir_with_parents(dir.c_str(), 0755);
    const std::string path =
        dir + "/" + std::to_string(g_str_hash(url.c_str()));

    if (g_file_test(path.c_str(), G_FILE_TEST_EXISTS)) {
      set_cover_file(path);
      return;
    }

    auto *job = new CoverJob{this, alive_, url, path, path + ".part"};
    const char *argv[] = {"curl", "-sfL",           "--max-time", "10",
                          "-o",   job->tmp.c_str(), url.c_str(),  nullptr};

    GError *err = nullptr;
    GSubprocess *proc = g_subprocess_newv(argv, G_SUBPROCESS_FLAGS_NONE, &err);
    if (!proc) {
      g_warning("cover: не удалось запустить curl: %s",
                err ? err->message : "?");
      g_clear_error(&err);
      delete job;
      return;
    }

    g_subprocess_wait_async(
        proc, nullptr,
        +[](GObject *src, GAsyncResult *res, gpointer data) {
          std::unique_ptr<CoverJob> job(static_cast<CoverJob *>(data));
          GSubprocess *p = G_SUBPROCESS(src);

          GError *e = nullptr;
          bool ok = g_subprocess_wait_finish(p, res, &e) &&
                    g_subprocess_get_successful(p);
          g_clear_error(&e);
          g_object_unref(p);

          if (!ok) {
            g_remove(job->tmp.c_str());
            return;
          }
          g_rename(job->tmp.c_str(), job->path.c_str());

          if (*job->alive && job->self->cover_url_ == job->url)
            job->self->set_cover_file(job->path);
        },
        job);
  }

  void start_animation() {
    if (tick_id_ != 0)
      return;

    last_time_ = 0;

    tick_id_ = area_.add_tick_callback(
        [this](const Glib::RefPtr<Gdk::FrameClock> &clock) {
          gint64 now = clock->get_frame_time();

          float dt = last_time_ == 0
                         ? 1.0f / 60.0f
                         : static_cast<float>(now - last_time_) / 1.0e6f;
          last_time_ = now;
          dt = std::min(dt, 1.0f / 30.0f);

          const int steps = 4;
          for (int i = 0; i < steps; ++i) {
            const float h = dt / steps;
            width_.step(h, 260.0f, 18.0f);
            height_.step(h, 220.0f, 15.0f);

            wob_w_.target = kJelly ? std::clamp(-width_.velocity * kWobbleGain,
                                                -kWobbleMax, kWobbleMax)
                                   : 0.0f;
            wob_h_.target = kJelly ? std::clamp(-height_.velocity * kWobbleGain,
                                                -kWobbleMax, kWobbleMax)
                                   : 0.0f;
            wob_w_.step(h, 220.0f, 9.0f);
            wob_h_.step(h, 220.0f, 9.0f);
          }

          wave_time_ += dt;
          const float wave_target =
              (kMusicFx != MusicFx::Off && state_ == island::State::Music)
                  ? 1.0f
                  : 0.0f;
          wave_amp_ += (wave_target - wave_amp_) * std::min(1.0f, dt * 4.0f);

          const bool moving = !(width_.settled() && height_.settled());

          if (moving || was_moving_) {
            if (!moving) {
              width_.value = width_.target;
              height_.value = height_.target;
              width_.velocity = height_.velocity = 0.0f;
            }
            update_content();
            apply_input_region();
          }
          was_moving_ = moving;

          area_.queue_draw();

          const bool wobbling = !(wob_w_.settled() && wob_h_.settled());
          const bool waving =
              kMusicFx != MusicFx::Off &&
              (wave_amp_ > 0.01f || state_ == island::State::Music);

          if (moving || wobbling || waving)
            return true;

          wob_w_ = {0.0f, 0.0f, 0.0f};
          wob_h_ = {0.0f, 0.0f, 0.0f};
          wave_amp_ = 0.0f;
          area_.queue_draw();
          tick_id_ = 0;
          return false;
        });
  }

  void update_content() {
    const auto target = island::size_for(state_);

    clip_.set_size_request(
        island::kMaxWidth,
        std::max(1, static_cast<int>(std::floor(height_.value))));

    const float fit =
        std::min(width_.value / target.w, height_.value / target.h);
    const float t = std::clamp((fit - 0.7f) / 0.2f, 0.0f, 1.0f);
    clip_.set_opacity(t * t);
  }

  struct Shape {
    float cx, w, h, r, f;
  };

  Shape current_shape(int area_width) const {
    float w =
        std::clamp(width_.value, 40.0f, static_cast<float>(island::kMaxWidth));
    float h =
        std::clamp(height_.value, 8.0f, static_cast<float>(island::kMaxHeight));

    Shape s;
    s.cx = area_width / 2.0f;
    s.w = w;
    s.h = h;
    s.r = std::min({island::kRadius, h / 2.0f, w / 2.0f});
    s.f = std::min(island::kFlare, h / 2.0f);
    return s;
  }

  static double peak_height(int i, double t) {
    const double a = std::sin(t * (2.1 + 0.43 * (i % 5)) + i * 1.9);
    const double b = std::sin(t * (1.3 + 0.31 * (i % 3)) + i * 0.7);
    const double v = std::clamp(0.5 + 0.5 * a * (0.6 + 0.4 * b), 0.0, 1.0);
    return v * v;
  }

  void on_draw(const Cairo::RefPtr<Cairo::Context> &cr, int width, int) {
    Shape s = current_shape(width);

    const double left = s.cx - s.w / 2.0;
    const double right = s.cx + s.w / 2.0;

    const bool peaks = kMusicFx == MusicFx::Peaks;
    const bool waves = kMusicFx == MusicFx::Waves;

    const double bulge = wob_w_.value;
    const double sag = wob_h_.value;
    const double amp = (peaks ? kPeakAmp : kWaveAmp) * wave_amp_;
    const double t = wave_time_ * (peaks ? kPeakSpeed : 1.0f);

    const double side_top = s.f;
    const double side_bot = s.h - s.r;
    const double side_len = side_bot - side_top;
    const double span = (right - s.r) - (left + s.r);

    // В режиме Waves боковые края тоже слегка колышутся
    const double bulge_r =
        bulge + (waves ? 0.6 * amp * std::sin(t * 1.7) : 0.0);
    const double bulge_l =
        bulge + (waves ? 0.6 * amp * std::sin(t * 1.7 + 2.5) : 0.0);

    cr->begin_new_path();
    cr->move_to(left - s.f, 0.0);
    cr->line_to(right + s.f, 0.0);
    cr->arc_negative(right + s.f, s.f, s.f, -M_PI / 2.0, -M_PI);

    cr->curve_to(right + bulge_r, side_top + side_len / 3.0, right + bulge_r,
                 side_top + 2.0 * side_len / 3.0, right, side_bot);
    cr->arc(right - s.r, s.h - s.r, s.r, 0.0, M_PI / 2.0);

    if (peaks) {
      // Точки ломаной чередуются: чётные лежат на кромке, нечётные - вершины
      // пиков. К углам пики сходят на нет, чтобы не торчали из скруглений
      const double x0 = right - s.r;
      const int bars =
          std::clamp(static_cast<int>(std::round(span / kPeakWidth)), 1, 32);
      const int pts = bars * 2;

      for (int j = 1; j <= pts; ++j) {
        const double u = static_cast<double>(j) / pts;
        double y = s.h + 3.0 * u * (1.0 - u) * sag;

        if (j % 2 == 1) {
          const double window = std::pow(std::sin(M_PI * u), 0.6);
          y += amp * window * peak_height(j / 2, t);
        }
        cr->line_to(x0 - span * u, y);
      }
    } else {
      const double sag1 = sag + (waves ? amp * std::sin(t * 2.4) : 0.0);
      const double sag2 = sag + (waves ? amp * std::sin(t * 2.4 + 2.2) : 0.0);
      cr->curve_to(right - s.r - span / 3.0, s.h + sag1,
                   right - s.r - 2.0 * span / 3.0, s.h + sag2, left + s.r, s.h);
    }
    cr->arc(left + s.r, s.h - s.r, s.r, M_PI / 2.0, M_PI);

    cr->curve_to(left - bulge_l, side_bot - side_len / 3.0, left - bulge_l,
                 side_bot - 2.0 * side_len / 3.0, left, s.f);
    cr->arc_negative(left - s.f, s.f, s.f, 0.0, -M_PI / 2.0);
    cr->close_path();

    const GdkRGBA &bg = themes_.rgba("bg");
    cr->set_source_rgb(bg.red, bg.green, bg.blue);
    cr->fill_preserve();

    cairo_set_line_join(cr->cobj(), CAIRO_LINE_JOIN_ROUND);
    cr->set_line_width(1.0);
    const GdkRGBA &ac = themes_.rgba("accent");
    cr->set_source_rgba(ac.red, ac.green, ac.blue, 0.14);
    cr->stroke();
  }

  void apply_input_region() {
    GdkSurface *surface = gtk_native_get_surface(GTK_NATIVE(gobj()));
    if (!surface)
      return;

    int width = area_.get_width();
    if (width <= 0)
      width = island::kMaxWidth + 2 * static_cast<int>(island::kFlare);

    Shape s = current_shape(width);

    cairo_rectangle_int_t rect;
    rect.x = static_cast<int>(std::floor(s.cx - s.w / 2.0f - s.f));
    rect.y = 0;
    rect.width = static_cast<int>(std::ceil(s.w + 2.0f * s.f));
    rect.height = static_cast<int>(std::ceil(s.h));

    cairo_region_t *region = cairo_region_create_rectangle(&rect);
    gdk_surface_set_input_region(surface, region);
    cairo_region_destroy(region);
  }

  MprisClient &mpris_;
  ThemeManager &themes_;

  Gtk::Overlay overlay_;
  Gtk::DrawingArea area_;
  Gtk::ScrolledWindow clip_;
  Gtk::Stack content_;

  ExpandedView *expanded_ = nullptr;

  Gtk::Label *clock_label_ = nullptr;
  Gtk::Label *compact_label_ = nullptr;
  Gtk::Label *full_title_ = nullptr;
  Gtk::Label *full_artist_ = nullptr;
  Gtk::Label *play_icon_ = nullptr;
  Gtk::Label *notif_title_ = nullptr;
  Gtk::Label *notif_body_ = nullptr;
  Gtk::Image *notif_icon_ = nullptr;
  Gtk::Picture *cover_small_ = nullptr;
  Gtk::Picture *cover_big_ = nullptr;

  island::Spring width_;
  island::Spring height_;
  island::Spring wob_w_;
  island::Spring wob_h_;
  float wave_amp_ = 0.0f;
  float wave_time_ = 0.0f;
  bool was_moving_ = false;

  island::State state_ = island::State::Idle;
  bool hovered_ = false;
  std::string cover_url_;

  std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
  sigc::connection leave_timer_;
  sigc::connection notif_timer_;
  sigc::connection clock_timer_;
  guint tick_id_ = 0;
  gint64 last_time_ = 0;
};
