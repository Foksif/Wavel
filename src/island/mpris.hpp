#pragma once

#include <gio/gio.h>
#include <sigc++/sigc++.h>

#include <algorithm>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

struct MediaInfo {
  std::string title;
  std::string artist;
  std::string art_url;
  std::string track_id;
  bool playing = false;

  std::string loop = "None"; // None / Track / Playlist
  bool shuffle = false;

  gint64 length_us = 0;
  gint64 position_us = 0;
  gint64 position_stamp = 0; // g_get_monotonic_time() в момент position_us
  double rate = 1.0;
};

class MprisClient {
public:
  ~MprisClient() {
    g_cancellable_cancel(cancellable_);
    g_object_unref(cancellable_);
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
        bus_, nullptr, kPlayer, "Seeked", kPath, nullptr,
        G_DBUS_SIGNAL_FLAGS_NONE, &MprisClient::on_seeked, this, nullptr));

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

  // Текущая позиция с учётом времени, прошедшего с последнего запроса
  gint64 position_now() const {
    const MediaInfo *m = current();
    return m ? extrapolate(*m) : 0;
  }

  // Асинхронно спрашивает у плеера реальную позицию (для ресинхронизации)
  void request_position() {
    std::string name;
    if (!bus_ || !current(&name))
      return;
    auto *req = new PosReq{this, name};
    g_dbus_connection_call(bus_, name.c_str(), kPath,
                           "org.freedesktop.DBus.Properties", "Get",
                           g_variant_new("(ss)", kPlayer, "Position"),
                           G_VARIANT_TYPE("(v)"), G_DBUS_CALL_FLAGS_NONE, 300,
                           cancellable_, &MprisClient::on_position_reply, req);
  }

  void seek_to(gint64 us) {
    std::string name;
    const MediaInfo *m = current(&name);
    if (!bus_ || !m)
      return;

    if (m->length_us > 0)
      us = std::min(us, m->length_us);
    us = std::max<gint64>(us, 0);

    if (g_variant_is_object_path(m->track_id.c_str()))
      call("SetPosition", g_variant_new("(ox)", m->track_id.c_str(), us));
    else // у плеера нет нормального trackid: двигаем относительно
      call("Seek", g_variant_new("(x)", us - extrapolate(*m)));

    MediaInfo &info = players_[name];
    info.position_us = us;
    info.position_stamp = g_get_monotonic_time();
  }

  // None -> Playlist -> Track -> None
  void cycle_loop() {
    std::string name;
    const MediaInfo *m = current(&name);
    if (!bus_ || !m)
      return;

    const char *next = m->loop == "None"       ? "Playlist"
                       : m->loop == "Playlist" ? "Track"
                                               : "None";
    set_property(name, "LoopStatus", g_variant_new_string(next));
    players_[name].loop = next;
    changed_.emit();
  }

  void toggle_shuffle() {
    std::string name;
    const MediaInfo *m = current(&name);
    if (!bus_ || !m)
      return;

    const bool on = !m->shuffle;
    set_property(name, "Shuffle", g_variant_new_boolean(on));
    players_[name].shuffle = on;
    changed_.emit();
  }

  sigc::signal<void()> &signal_changed() { return changed_; }

private:
  using Players = std::map<std::string, MediaInfo>;

  struct PosReq {
    MprisClient *self;
    std::string owner;
  };

  static constexpr const char *kPrefix = "org.mpris.MediaPlayer2.";
  static constexpr const char *kPath = "/org/mpris/MediaPlayer2";
  static constexpr const char *kPlayer = "org.mpris.MediaPlayer2.Player";

  static const MediaInfo *pick(Players::const_iterator it,
                               std::string *bus_name) {
    if (bus_name)
      *bus_name = it->first;
    return &it->second;
  }

  static gint64 extrapolate(const MediaInfo &i) {
    gint64 pos = i.position_us;
    if (i.playing) {
      const double rate = i.rate > 0.0 ? i.rate : 1.0;
      pos += static_cast<gint64>(
          static_cast<double>(g_get_monotonic_time() - i.position_stamp) *
          rate);
    }
    if (i.length_us > 0)
      pos = std::min(pos, i.length_us);
    return std::max<gint64>(pos, 0);
  }

  // params - floating GVariant, забирается вызовом
  void call(const char *method, GVariant *params = nullptr) {
    std::string name;
    if (!bus_ || !current(&name)) {
      if (params)
        g_variant_unref(g_variant_ref_sink(params));
      return;
    }
    g_dbus_connection_call(bus_, name.c_str(), kPath, kPlayer, method, params,
                           nullptr, G_DBUS_CALL_FLAGS_NONE, -1, nullptr,
                           nullptr, nullptr);
  }

  void set_property(const std::string &name, const char *prop,
                    GVariant *value) {
    g_dbus_connection_call(
        bus_, name.c_str(), kPath, "org.freedesktop.DBus.Properties", "Set",
        g_variant_new("(ssv)", kPlayer, prop, value), nullptr,
        G_DBUS_CALL_FLAGS_NONE, -1, nullptr, nullptr, nullptr);
  }

  static void apply_metadata(MediaInfo &info, GVariant *meta) {
    info.title.clear();
    info.artist.clear();
    info.art_url.clear();
    info.track_id.clear();
    info.length_us = 0;

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
      } else if (!strcmp(key, "mpris:trackid")) {
        if (g_variant_is_of_type(val, G_VARIANT_TYPE_OBJECT_PATH) ||
            g_variant_is_of_type(val, G_VARIANT_TYPE_STRING))
          info.track_id = g_variant_get_string(val, nullptr);
      } else if (!strcmp(key, "mpris:length")) {
        if (g_variant_is_of_type(val, G_VARIANT_TYPE_INT64))
          info.length_us = g_variant_get_int64(val);
        else if (g_variant_is_of_type(val, G_VARIANT_TYPE_UINT64))
          info.length_us = static_cast<gint64>(g_variant_get_uint64(val));
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
      } else if (!strcmp(key, "LoopStatus") &&
                 g_variant_is_of_type(val, G_VARIANT_TYPE_STRING)) {
        info.loop = g_variant_get_string(val, nullptr);
      } else if (!strcmp(key, "Shuffle") &&
                 g_variant_is_of_type(val, G_VARIANT_TYPE_BOOLEAN)) {
        info.shuffle = g_variant_get_boolean(val);
      } else if (!strcmp(key, "Rate") &&
                 g_variant_is_of_type(val, G_VARIANT_TYPE_DOUBLE)) {
        info.rate = g_variant_get_double(val);
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
      players_[owner_s].position_stamp = g_get_monotonic_time();
      return;
    }

    GVariant *dict = g_variant_get_child_value(all, 0);
    MediaInfo &info = players_[owner_s];
    apply_dict(info, dict);

    gint64 pos = 0;
    if (g_variant_lookup(dict, "Position", "x", &pos))
      info.position_us = pos;
    info.position_stamp = g_get_monotonic_time();

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

    // Фиксируем позицию до изменений: смена play/pause или трека её меняет
    const std::string old_track = info.track_id;
    const gint64 pos = extrapolate(info);

    apply_dict(info, changed);

    info.position_us = (info.track_id != old_track) ? 0 : pos;
    gint64 reported = 0;
    if (g_variant_lookup(changed, "Position", "x", &reported))
      info.position_us = reported;
    info.position_stamp = g_get_monotonic_time();

    if (info.playing)
      self->active_ = sender;
    g_variant_unref(changed);

    self->changed_.emit();
  }

  static void on_seeked(GDBusConnection *, const gchar *sender, const gchar *,
                        const gchar *, const gchar *, GVariant *params,
                        gpointer data) {
    auto *self = static_cast<MprisClient *>(data);
    gint64 pos = 0;
    g_variant_get(params, "(x)", &pos);

    MediaInfo &info = self->players_[sender];
    info.position_us = pos;
    info.position_stamp = g_get_monotonic_time();
  }

  static void on_position_reply(GObject *src, GAsyncResult *res,
                                gpointer data) {
    std::unique_ptr<PosReq> req(static_cast<PosReq *>(data));

    GError *e = nullptr;
    GVariant *r =
        g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, &e);
    g_clear_error(&e);
    if (!r) // ошибка или отмена: self мог уже уничтожиться, не трогаем
      return;

    GVariant *v = nullptr;
    g_variant_get(r, "(v)", &v);
    if (v && g_variant_is_of_type(v, G_VARIANT_TYPE_INT64)) {
      auto it = req->self->players_.find(req->owner);
      if (it != req->self->players_.end()) {
        it->second.position_us = g_variant_get_int64(v);
        it->second.position_stamp = g_get_monotonic_time();
      }
    }
    if (v)
      g_variant_unref(v);
    g_variant_unref(r);
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
  GCancellable *cancellable_ = g_cancellable_new();
  std::vector<guint> subs_;
  Players players_;
  std::string active_;
  sigc::signal<void()> changed_;
};
