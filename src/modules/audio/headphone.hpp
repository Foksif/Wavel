#pragma once

#include "audio_manager.hpp"

#include <glibmm/main.h>
#include <gtkmm/box.h>
#include <gtkmm/button.h>
#include <gtkmm/eventcontrollerscroll.h>
#include <gtkmm/label.h>
#include <sigc++/connection.h>

#include <string>

class HeadphoneControl : public Gtk::Box {
public:
  explicit HeadphoneControl(AudioManager &audio_manager)
      : Gtk::Box(Gtk::Orientation::HORIZONTAL, 6),
        audio_manager_(audio_manager),
        content_(Gtk::Orientation::HORIZONTAL, 6), icon_("󰕾"),
        volume_("100%") {
    icon_.add_css_class("audio-icon");
    volume_.add_css_class("audio-volume");

    content_.append(icon_);
    content_.append(volume_);

    button_.set_child(content_);
    button_.add_css_class("audio-button");
    button_.set_tooltip_text("Audio output");

    button_.signal_clicked().connect([this] {
      audio_manager_.set_output_muted(!audio_manager_.output_muted());
      update();
    });

    auto scroll = Gtk::EventControllerScroll::create();
    scroll->set_flags(Gtk::EventControllerScroll::Flags::VERTICAL);
    scroll->signal_scroll().connect(
        [this](double, double dy) {
          float v =
              audio_manager_.output_volume() - static_cast<float>(dy) * 0.05f;
          audio_manager_.set_output_volume(v);
          update();
          return true;
        },
        false);
    add_controller(scroll);

    append(button_);

    update_connection_ = Glib::signal_timeout().connect(
        [this] {
          update();
          return true;
        },
        250);

    update();
  }

  ~HeadphoneControl() override { update_connection_.disconnect(); }

private:
  void update() {
    if (audio_manager_.output_muted()) {
      volume_.set_text("mute");
      icon_.set_text("󰝟");
      icon_.add_css_class("muted");
      volume_.add_css_class("muted");
    } else {
      int percent =
          static_cast<int>(audio_manager_.output_volume() * 100.0f + 0.5f);
      volume_.set_text(std::to_string(percent) + "%");
      icon_.set_text("󰕾");
      icon_.remove_css_class("muted");
      volume_.remove_css_class("muted");
    }
  }

  AudioManager &audio_manager_;

  Gtk::Box content_;
  Gtk::Label icon_;
  Gtk::Button button_;
  Gtk::Label volume_;

  sigc::connection update_connection_;
};
