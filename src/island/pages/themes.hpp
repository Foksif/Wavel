#pragma once

#include "island/page.hpp"
#include "theme.hpp"

#include <gtkmm.h>

class ThemesPage : public IslandPage {
public:
  explicit ThemesPage(ThemeManager &themes)
      : IslandPage("themes", "\U000F03D8", "Темы"), themes_(themes) {
    title_label_.set_text("Темы");
    title_label_.set_halign(Gtk::Align::START);
    title_label_.add_css_class("island-page-title");

    list_.set_spacing(2);
    scroller_.set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::AUTOMATIC);
    scroller_.set_vexpand(true);
    scroller_.set_child(list_);

    append(title_label_);
    append(scroller_);

    themes_.signal_changed().connect(
        sigc::mem_fun(*this, &ThemesPage::schedule_rebuild));
    rebuild();
  }

  ~ThemesPage() override { idle_.disconnect(); }

  void on_shown() override { rebuild(); }

private:
  void schedule_rebuild() {
    idle_.disconnect();
    idle_ = Glib::signal_idle().connect([this] {
      rebuild();
      return false;
    });
  }
  void rebuild() {
    while (auto *c = list_.get_first_child())
      list_.remove(*c);

    for (const auto &t : themes_.list()) {
      const bool current = (t.id == themes_.current_id());

      auto *name = Gtk::make_managed<Gtk::Label>(t.name);
      name->set_halign(Gtk::Align::START);
      name->set_hexpand(true);
      name->set_ellipsize(Pango::EllipsizeMode::END);

      auto *mark = Gtk::make_managed<Gtk::Label>(current ? "\U000F012C" : "");
      mark->add_css_class("island-theme-mark");

      auto *box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
      box->append(*name);
      box->append(*mark);

      auto *row = Gtk::make_managed<Gtk::Button>();
      row->set_child(*box);
      row->set_has_frame(false);
      row->add_css_class("island-theme-row");
      if (current)
        row->add_css_class("current");
      row->signal_clicked().connect(
          [this, id = t.id] { themes_.set_theme(id); });

      list_.append(*row);
    }
  }

  ThemeManager &themes_;
  Gtk::Label title_label_;
  Gtk::ScrolledWindow scroller_;
  Gtk::Box list_{Gtk::Orientation::VERTICAL, 2};
  sigc::connection idle_;
};
