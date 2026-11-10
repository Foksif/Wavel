#pragma once

#include "island/mpris.hpp"
#include "island/page.hpp"

#include <gtkmm.h>

class HomePage : public IslandPage {
public:
  explicit HomePage(MprisClient &mpris)
      : IslandPage("home", "\U000F02DC", "Главная"), mpris_(mpris) {
    auto *inner = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 4);
    inner->set_vexpand(true);
    inner->set_valign(Gtk::Align::CENTER);

    clock_.add_css_class("island-home-clock");
    date_.add_css_class("island-home-date");
    media_.add_css_class("island-home-media");
    media_.set_ellipsize(Pango::EllipsizeMode::END);
    media_.set_max_width_chars(40);
    media_.set_margin_top(10);

    inner->append(clock_);
    inner->append(date_);
    inner->append(media_);
    append(*inner);

    mpris_.signal_changed().connect(
        sigc::mem_fun(*this, &HomePage::update_media));
    timer_ = Glib::signal_timeout().connect_seconds(
        [this] {
          update_clock();
          return true;
        },
        1);

    update_clock();
    update_media();
  }

  ~HomePage() override { timer_.disconnect(); }

  void on_shown() override {
    update_clock();
    update_media();
  }

private:
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
  sigc::connection timer_;
};
