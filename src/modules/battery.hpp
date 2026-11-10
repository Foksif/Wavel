#pragma once

#include "modules/module.hpp"

#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>

#include <cstdlib>
#include <cstring>
#include <string>

#include <glibmm/main.h>
#include <gtkmm/label.h>
#include <sigc++/functors/mem_fun.h>

class BatteryModule : public Module {
public:
  BatteryModule() : Module("battery") {
    battery_path_ = find_battery();

    icon_.add_css_class("battery-icon");
    percentage_.add_css_class("battery-percentage");

    append(icon_);
    append(percentage_);

    update();

    track(Glib::signal_timeout().connect(
        sigc::mem_fun(*this, &BatteryModule::update), 5000));
  }

private:
  static constexpr int kLowLevel = 15;

  static constexpr const char *kIcons[10] = {
      "\U000F007A", "\U000F007B", "\U000F007C", "\U000F007D", "\U000F007E",
      "\U000F007F", "\U000F0080", "\U000F0081", "\U000F0082", "\U000F0079"};

  static constexpr const char *kChargingIcons[10] = {
      "\U000F089C", "\U000F0086", "\U000F0087", "\U000F0088", "\U000F089D",
      "\U000F0089", "\U000F089E", "\U000F008A", "\U000F008B", "\U000F0085"};

  Gtk::Label icon_;
  Gtk::Label percentage_;

  std::string battery_path_;

  static int read_file(const char *path, char *buffer, size_t size) {
    int fd = open(path, O_RDONLY);
    if (fd < 0)
      return -1;

    ssize_t bytes = read(fd, buffer, size - 1);
    close(fd);

    if (bytes <= 0)
      return -1;

    buffer[bytes] = '\0';

    while (bytes > 0 &&
           (buffer[bytes - 1] == '\n' || buffer[bytes - 1] == '\r'))
      buffer[--bytes] = '\0';

    return static_cast<int>(bytes);
  }

  static std::string find_battery() {
    const char *root = "/sys/class/power_supply";

    DIR *dir = opendir(root);
    if (!dir)
      return {};

    std::string result;
    struct dirent *entry;

    while ((entry = readdir(dir)) != nullptr) {
      if (entry->d_name[0] == '.')
        continue;

      std::string base = std::string(root) + "/" + entry->d_name;
      std::string type_path = base + "/type";

      char type[32]{};
      if (read_file(type_path.c_str(), type, sizeof(type)) < 0)
        continue;

      if (std::strcmp(type, "Battery") == 0) {
        result = base;
        break;
      }
    }

    closedir(dir);
    return result;
  }

  int get_percentage() const {
    if (battery_path_.empty())
      return -1;

    std::string path = battery_path_ + "/capacity";

    char buffer[16]{};
    if (read_file(path.c_str(), buffer, sizeof(buffer)) < 0)
      return -1;

    return std::atoi(buffer);
  }

  bool is_charging() const {
    if (battery_path_.empty())
      return false;

    std::string path = battery_path_ + "/status";

    char buffer[32]{};
    if (read_file(path.c_str(), buffer, sizeof(buffer)) < 0)
      return false;

    return std::strcmp(buffer, "Charging") == 0;
  }

  bool update() {
    int percent = get_percentage();

    if (percent < 0) {
      icon_.set_text(kIcons[9]);
      percentage_.set_text("--");
      return true;
    }

    if (percent > 100)
      percent = 100;

    bool charging = is_charging();

    int index = percent / 10;
    if (index > 9)
      index = 9;

    icon_.set_text(charging ? kChargingIcons[index] : kIcons[index]);
    percentage_.set_text(std::to_string(percent) + "%");

    if (charging)
      add_css_class("charging");
    else
      remove_css_class("charging");

    if (!charging && percent <= kLowLevel)
      add_css_class("low");
    else
      remove_css_class("low");

    return true;
  }
};
