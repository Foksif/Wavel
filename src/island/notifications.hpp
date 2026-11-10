#pragma once

#include <gio/gio.h>
#include <sigc++/sigc++.h>

#include <cstring>
#include <string>

struct Notification {
  guint32 id = 0;
  std::string app;
  std::string icon;
  std::string summary;
  std::string body;
};

class NotificationServer {
public:
  ~NotificationServer() {
    if (owner_id_)
      g_bus_unown_name(owner_id_);
    if (node_)
      g_dbus_node_info_unref(node_);
  }

  void start() {
    if (owner_id_)
      return;

    GError *err = nullptr;
    node_ = g_dbus_node_info_new_for_xml(kXml, &err);
    if (!node_) {
      g_warning("Notifications: плохой XML: %s", err ? err->message : "?");
      g_clear_error(&err);
      return;
    }

    owner_id_ = g_bus_own_name(
        G_BUS_TYPE_SESSION, "org.freedesktop.Notifications",
        G_BUS_NAME_OWNER_FLAGS_NONE, &NotificationServer::on_bus_acquired,
        nullptr, &NotificationServer::on_name_lost, this, nullptr);
  }

  sigc::signal<void(const Notification &)> &signal_notification() {
    return signal_;
  }

private:
  static constexpr const char *kXml = R"(
<node>
  <interface name="org.freedesktop.Notifications">
    <method name="GetCapabilities">
      <arg type="as" direction="out"/>
    </method>
    <method name="Notify">
      <arg type="s" name="app_name" direction="in"/>
      <arg type="u" name="replaces_id" direction="in"/>
      <arg type="s" name="app_icon" direction="in"/>
      <arg type="s" name="summary" direction="in"/>
      <arg type="s" name="body" direction="in"/>
      <arg type="as" name="actions" direction="in"/>
      <arg type="a{sv}" name="hints" direction="in"/>
      <arg type="i" name="expire_timeout" direction="in"/>
      <arg type="u" direction="out"/>
    </method>
    <method name="CloseNotification">
      <arg type="u" name="id" direction="in"/>
    </method>
    <method name="GetServerInformation">
      <arg type="s" direction="out"/>
      <arg type="s" direction="out"/>
      <arg type="s" direction="out"/>
      <arg type="s" direction="out"/>
    </method>
  </interface>
</node>)";

  static void replace_all(std::string &s, const std::string &from,
                          const std::string &to) {
    size_t pos = 0;
    while ((pos = s.find(from, pos)) != std::string::npos) {
      s.replace(pos, from.size(), to);
      pos += to.size();
    }
  }

  static std::string strip_markup(const std::string &in) {
    std::string out;
    bool in_tag = false;
    for (char c : in) {
      if (c == '<')
        in_tag = true;
      else if (c == '>' && in_tag)
        in_tag = false;
      else if (!in_tag)
        out += c;
    }
    replace_all(out, "&lt;", "<");
    replace_all(out, "&gt;", ">");
    replace_all(out, "&quot;", "\"");
    replace_all(out, "&#39;", "'");
    replace_all(out, "&amp;", "&");
    return out;
  }

  static void on_bus_acquired(GDBusConnection *conn, const gchar *,
                              gpointer data) {
    auto *self = static_cast<NotificationServer *>(data);

    static const GDBusInterfaceVTable vtable = {
        &NotificationServer::on_method_call, nullptr, nullptr, {}};

    GError *err = nullptr;
    g_dbus_connection_register_object(conn, "/org/freedesktop/Notifications",
                                      self->node_->interfaces[0], &vtable, self,
                                      nullptr, &err);
    if (err) {
      g_warning("Notifications: register_object: %s", err->message);
      g_clear_error(&err);
    }
  }

  static void on_name_lost(GDBusConnection *, const gchar *, gpointer) {
    g_warning("Notifications: имя org.freedesktop.Notifications занято еблан");
  }

  static void on_method_call(GDBusConnection *, const gchar *, const gchar *,
                             const gchar *, const gchar *method,
                             GVariant *params, GDBusMethodInvocation *inv,
                             gpointer data) {
    auto *self = static_cast<NotificationServer *>(data);

    if (!strcmp(method, "GetCapabilities")) {
      GVariantBuilder b;
      g_variant_builder_init(&b, G_VARIANT_TYPE("as"));
      g_variant_builder_add(&b, "s", "body");
      g_dbus_method_invocation_return_value(inv, g_variant_new("(as)", &b));

    } else if (!strcmp(method, "GetServerInformation")) {
      g_dbus_method_invocation_return_value(
          inv, g_variant_new("(ssss)", "wavel", "foksif", "0.1", "1.2"));

    } else if (!strcmp(method, "CloseNotification")) {
      g_dbus_method_invocation_return_value(inv, nullptr);

    } else if (!strcmp(method, "Notify")) {
      const gchar *app, *icon, *summary, *body;
      guint32 replaces = 0;
      gint32 timeout = 0;
      GVariant *actions = nullptr, *hints = nullptr;

      g_variant_get(params, "(&su&s&s&s@as@a{sv}i)", &app, &replaces, &icon,
                    &summary, &body, &actions, &hints, &timeout);

      Notification n;
      n.id = replaces ? replaces : self->next_id_++;
      n.app = app;
      n.icon = icon;
      n.summary = strip_markup(summary);
      n.body = strip_markup(body);

      g_variant_unref(actions);
      g_variant_unref(hints);

      g_dbus_method_invocation_return_value(inv, g_variant_new("(u)", n.id));
      self->signal_.emit(n);

    } else {
      g_dbus_method_invocation_return_error(
          inv, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_METHOD, "Unknown method");
    }
  }

  GDBusNodeInfo *node_ = nullptr;
  guint owner_id_ = 0;
  guint32 next_id_ = 1;
  sigc::signal<void(const Notification &)> signal_;
};
