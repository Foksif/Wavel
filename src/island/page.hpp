#pragma once

#include <gtkmm.h>
#include <string>

class IslandPage : public Gtk::Box {
public:
  IslandPage(std::string id, std::string icon, std::string title)
      : Gtk::Box(Gtk::Orientation::VERTICAL, 8), id_(std::move(id)),
        icon_(std::move(icon)), title_(std::move(title)) {
    add_css_class("island-page");
  }

  const std::string &id() const { return id_; }
  const std::string &icon() const { return icon_; }
  const std::string &title() const { return title_; }

  virtual void on_shown() {}

private:
  std::string id_, icon_, title_;
};
