#pragma once

#include "island/page.hpp"

#include <functional>
#include <gtkmm.h>
#include <string>
#include <vector>

class ExpandedView : public Gtk::Box {
public:
  explicit ExpandedView(std::function<void()> on_close)
      : Gtk::Box(Gtk::Orientation::VERTICAL, 8) {
    add_css_class("island-expanded");
    set_margin_start(18);
    set_margin_end(18);
    set_margin_top(14);
    set_margin_bottom(12);

    stack_.set_hexpand(true);
    stack_.set_vexpand(true);
    stack_.set_transition_type(Gtk::StackTransitionType::CROSSFADE);
    stack_.set_transition_duration(150);

    tabs_.set_hexpand(true);
    tabs_.set_halign(Gtk::Align::START);

    auto *close = make_tab_button("\U000F0143");
    close->set_tooltip_text("Свернуть");
    close->signal_clicked().connect(std::move(on_close));

    bar_.add_css_class("island-tabbar");
    bar_.append(tabs_);
    bar_.append(*close);

    append(stack_);
    append(bar_);
  }

  void add_page(IslandPage &page) {
    stack_.add(page, Glib::ustring(page.id()));

    auto *btn = make_tab_button(page.icon().c_str());
    btn->set_tooltip_text(page.title());
    btn->signal_clicked().connect([this, id = page.id()] { show_page(id); });
    tabs_.append(*btn);

    tabs_list_.push_back({&page, btn});
    if (current_.empty())
      show_page(page.id());
  }

  void show_page(const std::string &id) {
    current_ = id;
    stack_.set_visible_child(Glib::ustring(id));
    for (auto &t : tabs_list_) {
      if (t.page->id() == id) {
        t.btn->add_css_class("active");
        t.page->on_shown();
      } else {
        t.btn->remove_css_class("active");
      }
    }
  }

  void refresh() { show_page(current_); }

private:
  struct Tab {
    IslandPage *page;
    Gtk::Button *btn;
  };

  static Gtk::Button *make_tab_button(const char *glyph) {
    auto *label = Gtk::make_managed<Gtk::Label>(glyph);
    label->add_css_class("island-tab-icon");
    auto *b = Gtk::make_managed<Gtk::Button>();
    b->set_child(*label);
    b->set_has_frame(false);
    b->add_css_class("island-tab");
    return b;
  }

  Gtk::Stack stack_;
  Gtk::Box bar_{Gtk::Orientation::HORIZONTAL, 4};
  Gtk::Box tabs_{Gtk::Orientation::HORIZONTAL, 4};
  std::vector<Tab> tabs_list_;
  std::string current_;
};
