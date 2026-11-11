#pragma once

#include "../../layout.hpp"
#include "island/page.hpp"

#include <gtkmm.h>
#include <map>

class LayoutPage : public IslandPage {
public:
  explicit LayoutPage(LayoutManager &layout)
      : IslandPage("layout", "\U000F0493", "Модули"), layout_(layout) {
    auto *cols = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 10);
    cols->set_homogeneous(true);
    cols->set_vexpand(true);
    cols->set_valign(Gtk::Align::CENTER);

    const char *titles[3] = {"Слева", "Скрыто", "Справа"};
    for (int i = 0; i < 3; ++i) {
      auto *wrap = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 6);
      auto *title = Gtk::make_managed<Gtk::Label>(titles[i]);
      title->add_css_class("island-col-title");
      title->set_halign(Gtk::Align::START);

      col_[i] = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 4);
      col_[i]->set_valign(Gtk::Align::START);

      wrap->append(*title);
      wrap->append(*col_[i]);
      cols->append(*wrap);
    }
    append(*cols);

    for (auto &m : LayoutManager::kModules)
      chips_[m.id] = make_chip(m.id, m.title);

    layout_.signal_changed().connect(
        sigc::mem_fun(*this, &LayoutPage::queue_refresh));
    refresh();
  }

  void on_shown() override { refresh(); }

private:
  struct Chip {
    Gtk::Box *box;
    Gtk::Button *left, *up, *right;
  };

  Gtk::Button *mini(const char *glyph, std::function<void()> cb) {
    auto *icon = Gtk::make_managed<Gtk::Label>(glyph);
    icon->add_css_class("island-chip-icon");
    auto *b = Gtk::make_managed<Gtk::Button>();
    b->set_child(*icon);
    b->set_has_frame(false);
    b->add_css_class("island-chip-btn");
    b->signal_clicked().connect(std::move(cb));
    return b;
  }

  Chip make_chip(const std::string &id, const char *title) {
    Chip c;
    c.box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 2);
    c.box->add_css_class("island-chip");

    c.left = mini("\U000F0141", [this, id] { layout_.move(id, -1); });
    c.up = mini("\U000F0143", [this, id] { layout_.bump(id); });
    c.right = mini("\U000F0142", [this, id] { layout_.move(id, +1); });

    auto *label = Gtk::make_managed<Gtk::Label>(title);
    label->add_css_class("island-chip-label");
    label->set_hexpand(true);
    label->set_halign(Gtk::Align::START);
    label->set_ellipsize(Pango::EllipsizeMode::END);

    c.box->append(*c.left);
    c.box->append(*label);
    c.box->append(*c.up);
    c.box->append(*c.right);
    return c;
  }

  void place(const std::vector<std::string> &ids, int col) {
    for (auto &id : ids) {
      auto &c = chips_.at(id);
      reparent(*c.box, *col_[col]);
      c.left->set_visible(col != LayoutManager::Left);
      c.up->set_visible(col != LayoutManager::Hidden);
      c.right->set_visible(col != LayoutManager::Right);
      if (col == LayoutManager::Hidden)
        c.box->add_css_class("off");
      else
        c.box->remove_css_class("off");
    }
  }

  void refresh() {
    place(layout_.left(), LayoutManager::Left);
    place(layout_.hidden(), LayoutManager::Hidden);
    place(layout_.right(), LayoutManager::Right);
  }

  void queue_refresh() {
    Glib::signal_idle().connect_once([this] { refresh(); });
  }

  LayoutManager &layout_;
  Gtk::Box *col_[3] = {};
  std::map<std::string, Chip> chips_;
};
