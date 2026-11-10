#pragma once
#include <gtkmm.h>
#include <vector>

class Module : public Gtk::Box {
public:
  explicit Module(const Glib::ustring &name)
      : Gtk::Box(Gtk::Orientation::HORIZONTAL, 6) {
    add_css_class("module");
    add_css_class(name);
  }

  ~Module() override {
    for (auto &c : connections_)
      c.disconnect();
  }

protected:
  void track(sigc::connection c) { connections_.push_back(std::move(c)); }

private:
  std::vector<sigc::connection> connections_;
};
