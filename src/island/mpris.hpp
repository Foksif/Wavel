#pragma once

#include <gio/gio.h>
#include <sigc++/sigc++.h>

#include <cstring>
#include <map>
#include <string>
#include <vector>

struct MediaInfo {
  std::string title;
  std::string artist;
  std::string art_url;
  bool playing = false;
};

class MprisClient {
public:
  ~MprisClient() {
    if (!bus_)
      return;
    for (guint id : subs_)
      g_dbus_connection_signal_unsubscribe(bus_, id);
    g_object_unref(bus_);
  }

  void start() {
    if (bus_)
      return;

    GError *err = nullptr;
    bus_ = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &err);
    if (!bus_) {
      g_warning("MPRIS: нет session bus: %s", err ? err->message : "?");
      g_clear_error(&err);
      return;
    }

    subs_.push_back(g_dbus_connection_signal_subscribe(
        bus_, nullptr, "org.freedesktop.DBus.Properties", "PropertiesChanged",
        kPath, kPlayer, G_DBUS_SIGNAL_FLAGS_NONE,
        &MprisClient::on_properties_changed, this, nullptr));

    subs_.push_back(g_dbus_connection_signal_subscribe(
        bus_, "org.freedesktop.DBus", "org.freedesktop.DBus",
        "NameOwnerChanged", "/org/freedesktop/DBus", nullptr,
        G_DBUS_SIGNAL_FLAGS_NONE, &MprisClient::on_name_owner_changed, this,
        nullptr));

    scan_players();
  }

  const MediaInfo *current(std::string *bus_name = nullptr) const {
    auto it = players_.find(active_);
    if (it != players_.end() && it->second.playing)
      return pick(it, bus_name);

    for (auto i = players_.begin(); i != players_.end(); ++i)
      if (i->second.playing)
        return pick(i, bus_name);

    if (it != players_.end())
      return pick(it, bus_name);
    if (!players_.empty())
      return pick(players_.begin(), bus_name);
    return nullptr;
  }

  void play_pause() { call("PlayPause"); }
  void next() { call("Next"); }
  void previous() { call("Previous"); }

  sigc::signal<void()> &signal_changed() { return changed_; }

private:
  using Players = std::map<std::string, MediaInfo>;

  static constexpr const char *kPrefix = "org.mpris.MediaPlayer2.";
  static constexpr const char *kPath = "/org/mpris/MediaPlayer2";
  static constexpr const char *kPlayer = "org.mpris.MediaPlayer2.Player";

  static const MediaInfo *pick(Players::const_iterator it,
                               std::string *bus_name) {
    if (bus_name)
      *bus_name = it->first;
    return &it->second;
  }

  void call(const char *method) {
    std::string name;
    if (!bus_ || !current(&name))
      return;
    g_dbus_connection_call(bus_, name.c_str(), kPath, kPlayer, method, nullptr,
                           nullptr, G_DBUS_CALL_FLAGS_NONE, -1, nullptr,
                           nullptr, nullptr);
  }

  static void apply_metadata(MediaInfo &info, GVariant *meta) {
    info.title.clear();
    info.artist.clear();
    info.art_url.clear();

    GVariantIter it;
    g_variant_iter_init(&it, meta);
    const gchar *key;
    GVariant *val;
    while (g_variant_iter_next(&it, "{&sv}", &key, &val)) {
      if (!strcmp(key, "xesam:title") &&
          g_variant_is_of_type(val, G_VARIANT_TYPE_STRING)) {
        info.title = g_variant_get_string(val, nullptr);
      } else if (!strcmp(key, "mpris:artUrl") &&
                 g_variant_is_of_type(val, G_VARIANT_TYPE_STRING)) {
        info.art_url = g_variant_get_string(val, nullptr);
      } else if (!strcmp(key, "xesam:artist")) {
        if (g_variant_is_of_type(val, G_VARIANT_TYPE_STRING_ARRAY)) {
          gsize n = 0;
          const gchar **arr = g_variant_get_strv(val, &n);
          for (gsize i = 0; i < n; ++i) {
            if (i)
              info.artist += ", ";
            info.artist += arr[i];
          }
          g_free(arr);
        } else if (g_variant_is_of_type(val, G_VARIANT_TYPE_STRING)) {
          info.artist = g_variant_get_string(val, nullptr);
        }
      }
      g_variant_unref(val);
    }
  }

  static void apply_dict(MediaInfo &info, GVariant *dict) {
    GVariantIter it;
    g_variant_iter_init(&it, dict);
    const gchar *key;
    GVariant *val;
    while (g_variant_iter_next(&it, "{&sv}", &key, &val)) {
      if (!strcmp(key, "PlaybackStatus") &&
          g_variant_is_of_type(val, G_VARIANT_TYPE_STRING)) {
        info.playing =
            g_strcmp0(g_variant_get_string(val, nullptr), "Playing") == 0;
      } else if (!strcmp(key, "Metadata")) {
        apply_metadata(info, val);
      }
      g_variant_unref(val);
    }
  }

  void scan_players() {
    GVariant *r = g_dbus_connection_call_sync(
        bus_, "org.freedesktop.DBus", "/org/freedesktop/DBus",
        "org.freedesktop.DBus", "ListNames", nullptr, G_VARIANT_TYPE("(as)"),
        G_DBUS_CALL_FLAGS_NONE, 500, nullptr, nullptr);
    if (!r)
      return;

    GVariant *arr = g_variant_get_child_value(r, 0);
    GVariantIter it;
    g_variant_iter_init(&it, arr);
    const gchar *name;
    while (g_variant_iter_next(&it, "&s", &name))
      if (g_str_has_prefix(name, kPrefix))
        add_player(name);

    g_variant_unref(arr);
    g_variant_unref(r);
    changed_.emit();
  }

  void add_player(const std::string &name) {
    GVariant *o = g_dbus_connection_call_sync(
        bus_, "org.freedesktop.DBus", "/org/freedesktop/DBus",
        "org.freedesktop.DBus", "GetNameOwner",
        g_variant_new("(s)", name.c_str()), G_VARIANT_TYPE("(s)"),
        G_DBUS_CALL_FLAGS_NONE, 500, nullptr, nullptr);
    if (!o)
      return;
    const gchar *owner = nullptr;
    g_variant_get(o, "(&s)", &owner);
    std::string owner_s = owner ? owner : "";
    g_variant_unref(o);
    if (owner_s.empty())
      return;

    GVariant *all = g_dbus_connection_call_sync(
        bus_, name.c_str(), kPath, "org.freedesktop.DBus.Properties", "GetAll",
        g_variant_new("(s)", kPlayer), G_VARIANT_TYPE("(a{sv})"),
        G_DBUS_CALL_FLAGS_NONE, 500, nullptr, nullptr);
    if (!all) {
      players_[owner_s];
      return;
    }

    GVariant *dict = g_variant_get_child_value(all, 0);
    MediaInfo &info = players_[owner_s];
    apply_dict(info, dict);
    if (info.playing)
      active_ = owner_s;
    g_variant_unref(dict);
    g_variant_unref(all);
  }

  static void on_properties_changed(GDBusConnection *, const gchar *sender,
                                    const gchar *, const gchar *, const gchar *,
                                    GVariant *params, gpointer data) {
    auto *self = static_cast<MprisClient *>(data);

    GVariant *changed = g_variant_get_child_value(params, 1);
    MediaInfo &info = self->players_[sender];
    apply_dict(info, changed);
    if (info.playing)
      self->active_ = sender;
    g_variant_unref(changed);

    self->changed_.emit();
  }

  static void on_name_owner_changed(GDBusConnection *, const gchar *,
                                    const gchar *, const gchar *, const gchar *,
                                    GVariant *params, gpointer data) {
    auto *self = static_cast<MprisClient *>(data);

    const gchar *name, *old_owner, *new_owner;
    g_variant_get(params, "(&s&s&s)", &name, &old_owner, &new_owner);
    if (!g_str_has_prefix(name, kPrefix))
      return;

    if (*old_owner)
      self->players_.erase(old_owner);
    if (*new_owner)
      self->add_player(name);

    self->changed_.emit();
  }

  GDBusConnection *bus_ = nullptr;
  std::vector<guint> subs_;
  Players players_;
  std::string active_;
  sigc::signal<void()> changed_;
};
