#pragma once

#include "module.hpp"

#include <gio/gio.h>
#include <glib.h>
#include <gtk/gtk.h>
#include <unistd.h>

#include <cstring>
#include <ctime>
#include <memory>
#include <string>
#include <vector>

#include <gtkmm/box.h>
#include <gtkmm/gestureclick.h>
#include <gtkmm/image.h>
#include <gtkmm/picture.h>

namespace tray {
constexpr const char *kWatcherName = "org.kde.StatusNotifierWatcher";
constexpr const char *kWatcherPath = "/StatusNotifierWatcher";
constexpr const char *kItemIface = "org.kde.StatusNotifierItem";
constexpr const char *kMenuIface = "com.canonical.dbusmenu";
constexpr int kIconSize = 16;
} // namespace tray

class TrayItem : public Gtk::Box {
public:
  TrayItem(GDBusConnection *connection, const std::string &id)
      : Gtk::Box(Gtk::Orientation::HORIZONTAL), connection_(connection),
        id_(id) {
    size_t slash = id.find('/');
    bus_name_ = id.substr(0, slash);
    path_ =
        slash == std::string::npos ? "/StatusNotifierItem" : id.substr(slash);

    add_css_class("tray-item");

    image_.set_pixel_size(tray::kIconSize);

    picture_.set_size_request(tray::kIconSize, tray::kIconSize);
    gtk_picture_set_can_shrink(picture_.gobj(), TRUE);
    picture_.set_visible(false);

    append(image_);
    append(picture_);

    cancellable_ = g_cancellable_new();

    signal_id_ = g_dbus_connection_signal_subscribe(
        connection_, bus_name_.c_str(), tray::kItemIface, nullptr,
        path_.c_str(), nullptr, G_DBUS_SIGNAL_FLAGS_NONE, on_item_signal, this,
        nullptr);

    auto click = Gtk::GestureClick::create();
    click->set_button(0);
    Gtk::GestureClick *raw = click.get();
    click->signal_pressed().connect([this, raw](int, double, double) {
      on_click(raw->get_current_button());
    });
    add_controller(click);

    refresh();
  }

  ~TrayItem() override {
    g_cancellable_cancel(cancellable_);
    g_dbus_connection_signal_unsubscribe(connection_, signal_id_);
    g_object_unref(cancellable_);

    if (popover_) {
      gtk_widget_unparent(popover_);
      popover_ = nullptr;
    }
  }

  const std::string &id() const { return id_; }

private:
  struct MenuAction {
    TrayItem *item;
    int id;
  };

  void refresh() {
    g_dbus_connection_call(connection_, bus_name_.c_str(), path_.c_str(),
                           "org.freedesktop.DBus.Properties", "GetAll",
                           g_variant_new("(s)", tray::kItemIface),
                           G_VARIANT_TYPE("(a{sv})"), G_DBUS_CALL_FLAGS_NONE,
                           -1, cancellable_, on_properties, this);
  }

  static void on_item_signal(GDBusConnection *, const gchar *, const gchar *,
                             const gchar *, const gchar *signal_name,
                             GVariant *, gpointer data) {
    if (std::strncmp(signal_name, "New", 3) == 0)
      static_cast<TrayItem *>(data)->refresh();
  }

  static void on_properties(GObject *source, GAsyncResult *result,
                            gpointer data) {
    GError *error = nullptr;
    GVariant *reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source),
                                                    result, &error);

    if (!reply) {
      g_error_free(error);
      return;
    }

    static_cast<TrayItem *>(data)->apply_properties(reply);
    g_variant_unref(reply);
  }

  static std::string lookup_string(GVariant *props, const char *key) {
    const char *value = nullptr;
    if (g_variant_lookup(props, key, "&s", &value) && value)
      return value;
    return {};
  }

  void apply_properties(GVariant *reply) {
    GVariant *props = nullptr;
    g_variant_get(reply, "(@a{sv})", &props);

    status_ = lookup_string(props, "Status");

    std::string theme_path = lookup_string(props, "IconThemePath");
    if (!theme_path.empty() && theme_path != theme_path_) {
      theme_path_ = theme_path;
      gtk_icon_theme_add_search_path(
          gtk_icon_theme_get_for_display(gdk_display_get_default()),
          theme_path_.c_str());
    }

    const char *menu = nullptr;
    menu_path_.clear();
    if (g_variant_lookup(props, "Menu", "&o", &menu) && menu &&
        std::strcmp(menu, "/NO_DBUSMENU") != 0)
      menu_path_ = menu;

    gboolean is_menu = FALSE;
    item_is_menu_ =
        g_variant_lookup(props, "ItemIsMenu", "b", &is_menu) && is_menu;

    const bool attention = status_ == "NeedsAttention";

    std::string icon_name =
        lookup_string(props, attention ? "AttentionIconName" : "IconName");
    GVariant *pixmaps = g_variant_lookup_value(
        props, attention ? "AttentionIconPixmap" : "IconPixmap",
        G_VARIANT_TYPE("a(iiay)"));

    if (icon_name.empty() && (!pixmaps || g_variant_n_children(pixmaps) == 0)) {
      icon_name = lookup_string(props, "IconName");
      if (pixmaps)
        g_variant_unref(pixmaps);
      pixmaps = g_variant_lookup_value(props, "IconPixmap",
                                       G_VARIANT_TYPE("a(iiay)"));
    }

    apply_icon(icon_name, pixmaps);

    if (pixmaps)
      g_variant_unref(pixmaps);

    std::string tip;
    GVariant *tooltip = g_variant_lookup_value(props, "ToolTip",
                                               G_VARIANT_TYPE("(sa(iiay)ss)"));
    if (tooltip) {
      const char *title = nullptr;
      g_variant_get_child(tooltip, 2, "&s", &title);
      if (title)
        tip = title;
      g_variant_unref(tooltip);
    }
    if (tip.empty())
      tip = lookup_string(props, "Title");
    set_tooltip_text(tip);

    set_visible(status_ != "Passive");

    if (attention)
      add_css_class("attention");
    else
      remove_css_class("attention");

    g_variant_unref(props);
  }

  void show_image(bool named) {
    image_.set_visible(named);
    picture_.set_visible(!named);
  }

  void apply_icon(const std::string &name, GVariant *pixmaps) {
    if (!name.empty()) {
      if (name[0] == '/') {
        GError *error = nullptr;
        GdkTexture *texture =
            gdk_texture_new_from_filename(name.c_str(), &error);
        if (texture) {
          gtk_picture_set_paintable(picture_.gobj(), GDK_PAINTABLE(texture));
          g_object_unref(texture);
          show_image(false);
          return;
        }
        g_error_free(error);
      } else {
        gtk_image_set_from_icon_name(image_.gobj(), name.c_str());
        show_image(true);
        return;
      }
    }

    if (pixmaps) {
      GdkTexture *texture = texture_from_pixmaps(pixmaps);
      if (texture) {
        gtk_picture_set_paintable(picture_.gobj(), GDK_PAINTABLE(texture));
        g_object_unref(texture);
        show_image(false);
        return;
      }
    }

    gtk_image_set_from_icon_name(image_.gobj(), "image-missing");
    show_image(true);
  }

  static GdkTexture *texture_from_pixmaps(GVariant *pixmaps) {
    GVariantIter iter;
    g_variant_iter_init(&iter, pixmaps);

    int best_w = 0, best_h = 0;
    GVariant *best_data = nullptr;

    GVariant *child;
    while ((child = g_variant_iter_next_value(&iter)) != nullptr) {
      int w = 0, h = 0;
      GVariant *data = nullptr;
      g_variant_get(child, "(ii@ay)", &w, &h, &data);

      gsize size = 0;
      g_variant_get_fixed_array(data, &size, 1);

      if (w > 0 && h > 0 && w > best_w &&
          size >= static_cast<gsize>(w) * static_cast<gsize>(h) * 4) {
        if (best_data)
          g_variant_unref(best_data);
        best_data = data;
        best_w = w;
        best_h = h;
      } else {
        g_variant_unref(data);
      }

      g_variant_unref(child);
    }

    if (!best_data)
      return nullptr;

    GBytes *bytes = g_variant_get_data_as_bytes(best_data);
    GdkTexture *texture =
        gdk_memory_texture_new(best_w, best_h, GDK_MEMORY_A8R8G8B8, bytes,
                               static_cast<gsize>(best_w) * 4);

    g_bytes_unref(bytes);
    g_variant_unref(best_data);
    return texture;
  }

  void on_click(guint button) {
    if (button == 1) {
      if (item_is_menu_)
        show_menu();
      else
        call_item_method("Activate");
    } else if (button == 2) {
      call_item_method("SecondaryActivate");
    } else if (button == 3) {
      if (!menu_path_.empty())
        show_menu();
      else
        call_item_method("ContextMenu");
    }
  }

  void call_item_method(const char *method) {
    g_dbus_connection_call(
        connection_, bus_name_.c_str(), path_.c_str(), tray::kItemIface, method,
        g_variant_new("(ii)", 0, 0), nullptr, G_DBUS_CALL_FLAGS_NONE, -1,
        nullptr, nullptr, nullptr);
  }

  void show_menu() {
    g_dbus_connection_call(
        connection_, bus_name_.c_str(), menu_path_.c_str(), tray::kMenuIface,
        "AboutToShow", g_variant_new("(i)", 0), G_VARIANT_TYPE("(b)"),
        G_DBUS_CALL_FLAGS_NONE, 1000, cancellable_, on_about_to_show, this);
  }

  static void on_about_to_show(GObject *source, GAsyncResult *result,
                               gpointer data) {
    GError *error = nullptr;
    GVariant *reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source),
                                                    result, &error);

    if (reply) {
      g_variant_unref(reply);
    } else {
      bool cancelled = g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
      g_error_free(error);
      if (cancelled)
        return;
    }

    static_cast<TrayItem *>(data)->request_layout();
  }

  void request_layout() {
    const gchar *const no_properties[] = {nullptr};

    g_dbus_connection_call(
        connection_, bus_name_.c_str(), menu_path_.c_str(), tray::kMenuIface,
        "GetLayout", g_variant_new("(ii^as)", 0, -1, no_properties),
        G_VARIANT_TYPE("(u(ia{sv}av))"), G_DBUS_CALL_FLAGS_NONE, 2000,
        cancellable_, on_layout, this);
  }

  static void on_layout(GObject *source, GAsyncResult *result, gpointer data) {
    GError *error = nullptr;
    GVariant *reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source),
                                                    result, &error);

    if (!reply) {
      g_error_free(error);
      return;
    }

    static_cast<TrayItem *>(data)->popup_menu(reply);
    g_variant_unref(reply);
  }

  static void on_ctx_free(gpointer data, GClosure *) { g_free(data); }

  static void on_menu_activate(GSimpleAction *, GVariant *, gpointer data) {
    auto *ctx = static_cast<MenuAction *>(data);
    ctx->item->send_event(ctx->id);
  }

  void send_event(int id) {
    g_dbus_connection_call(
        connection_, bus_name_.c_str(), menu_path_.c_str(), tray::kMenuIface,
        "Event",
        g_variant_new("(isvu)", id, "clicked", g_variant_new_int32(0),
                      static_cast<guint32>(std::time(nullptr))),
        nullptr, G_DBUS_CALL_FLAGS_NONE, -1, nullptr, nullptr, nullptr);
  }

  GMenu *build_menu(GVariant *children, GSimpleActionGroup *group) {
    GMenu *top = g_menu_new();
    GMenu *section = g_menu_new();

    GVariantIter iter;
    g_variant_iter_init(&iter, children);

    GVariant *boxed;
    while ((boxed = g_variant_iter_next_value(&iter)) != nullptr) {
      GVariant *node = g_variant_get_variant(boxed);

      int id = 0;
      GVariant *props = nullptr;
      GVariant *sub = nullptr;
      g_variant_get(node, "(i@a{sv}@av)", &id, &props, &sub);

      const char *type = "";
      g_variant_lookup(props, "type", "&s", &type);

      gboolean visible = TRUE;
      g_variant_lookup(props, "visible", "b", &visible);

      if (std::strcmp(type, "separator") == 0) {
        if (g_menu_model_get_n_items(G_MENU_MODEL(section)) > 0) {
          g_menu_append_section(top, nullptr, G_MENU_MODEL(section));
          g_object_unref(section);
          section = g_menu_new();
        }
      } else if (visible) {
        const char *label = "";
        g_variant_lookup(props, "label", "&s", &label);

        gboolean enabled = TRUE;
        g_variant_lookup(props, "enabled", "b", &enabled);

        const char *toggle_type = "";
        g_variant_lookup(props, "toggle-type", "&s", &toggle_type);

        int toggle_state = 0;
        g_variant_lookup(props, "toggle-state", "i", &toggle_state);

        std::string text = label;
        if (toggle_type[0] && toggle_state == 1)
          text = "✓ " + text;

        if (g_variant_n_children(sub) > 0) {
          GMenu *submenu = build_menu(sub, group);
          g_menu_append_submenu(section, text.c_str(), G_MENU_MODEL(submenu));
          g_object_unref(submenu);
        } else {
          std::string action = "item-" + std::to_string(id);

          GSimpleAction *act = g_simple_action_new(action.c_str(), nullptr);
          g_simple_action_set_enabled(act, enabled);

          auto *ctx = g_new(MenuAction, 1);
          ctx->item = this;
          ctx->id = id;

          g_signal_connect_data(act, "activate", G_CALLBACK(on_menu_activate),
                                ctx, on_ctx_free,
                                static_cast<GConnectFlags>(0));

          g_action_map_add_action(G_ACTION_MAP(group), G_ACTION(act));
          g_object_unref(act);

          std::string detailed = "tray." + action;
          g_menu_append(section, text.c_str(), detailed.c_str());
        }
      }

      g_variant_unref(props);
      g_variant_unref(sub);
      g_variant_unref(node);
      g_variant_unref(boxed);
    }

    if (g_menu_model_get_n_items(G_MENU_MODEL(section)) > 0)
      g_menu_append_section(top, nullptr, G_MENU_MODEL(section));
    g_object_unref(section);

    return top;
  }

  void popup_menu(GVariant *reply) {
    GVariant *root = nullptr;
    g_variant_get(reply, "(u@(ia{sv}av))", nullptr, &root);

    GVariant *children = nullptr;
    g_variant_get(root, "(i@a{sv}@av)", nullptr, nullptr, &children);

    GSimpleActionGroup *group = g_simple_action_group_new();
    GMenu *menu = build_menu(children, group);

    g_variant_unref(children);
    g_variant_unref(root);

    if (g_menu_model_get_n_items(G_MENU_MODEL(menu)) == 0) {
      g_object_unref(menu);
      g_object_unref(group);
      return;
    }

    if (popover_) {
      gtk_widget_unparent(popover_);
      popover_ = nullptr;
    }

    GtkWidget *popover = gtk_popover_menu_new_from_model(G_MENU_MODEL(menu));
    gtk_widget_insert_action_group(popover, "tray", G_ACTION_GROUP(group));
    gtk_popover_set_has_arrow(GTK_POPOVER(popover), FALSE);
    gtk_widget_set_parent(popover, GTK_WIDGET(gobj()));

    g_object_unref(menu);
    g_object_unref(group);

    popover_ = popover;
    gtk_popover_popup(GTK_POPOVER(popover_));
  }

  GDBusConnection *connection_;
  GCancellable *cancellable_ = nullptr;
  guint signal_id_ = 0;

  std::string id_;
  std::string bus_name_;
  std::string path_;
  std::string menu_path_;
  std::string status_;
  std::string theme_path_;
  bool item_is_menu_ = false;

  Gtk::Image image_;
  Gtk::Picture picture_;
  GtkWidget *popover_ = nullptr;
};

class TrayModule : public Module {
public:
  TrayModule() : Module("tray") {
    set_visible(false);

    connection_ = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, nullptr);
    if (!connection_)
      return;

    cancellable_ = g_cancellable_new();

    start_watcher();
    start_host();
  }

  ~TrayModule() override {
    if (cancellable_)
      g_cancellable_cancel(cancellable_);

    for (auto &item : items_)
      remove(*item);
    items_.clear();

    if (!connection_)
      return;

    if (client_signal_id_)
      g_dbus_connection_signal_unsubscribe(connection_, client_signal_id_);
    if (watch_id_)
      g_bus_unwatch_name(watch_id_);
    if (host_own_id_)
      g_bus_unown_name(host_own_id_);

    for (WatchedItem *w : watched_) {
      g_bus_unwatch_name(w->watch_id);
      delete w;
    }
    watched_.clear();

    if (watcher_object_id_)
      g_dbus_connection_unregister_object(connection_, watcher_object_id_);
    if (watcher_own_id_)
      g_bus_unown_name(watcher_own_id_);
    if (node_info_)
      g_dbus_node_info_unref(node_info_);

    g_object_unref(cancellable_);
    g_object_unref(connection_);
  }

  TrayModule(const TrayModule &) = delete;
  TrayModule &operator=(const TrayModule &) = delete;

private:
  struct WatchedItem {
    TrayModule *self;
    std::string id;
    guint watch_id;
  };

  static constexpr const char *kWatcherXml =
      "<node>"
      "  <interface name='org.kde.StatusNotifierWatcher'>"
      "    <method name='RegisterStatusNotifierItem'>"
      "      <arg type='s' name='service' direction='in'/>"
      "    </method>"
      "    <method name='RegisterStatusNotifierHost'>"
      "      <arg type='s' name='service' direction='in'/>"
      "    </method>"
      "    <property name='RegisteredStatusNotifierItems' type='as' "
      "access='read'/>"
      "    <property name='IsStatusNotifierHostRegistered' type='b' "
      "access='read'/>"
      "    <property name='ProtocolVersion' type='i' access='read'/>"
      "    <signal name='StatusNotifierItemRegistered'>"
      "      <arg type='s'/>"
      "    </signal>"
      "    <signal name='StatusNotifierItemUnregistered'>"
      "      <arg type='s'/>"
      "    </signal>"
      "    <signal name='StatusNotifierHostRegistered'/>"
      "    <signal name='StatusNotifierHostUnregistered'/>"
      "  </interface>"
      "</node>";

  static void watcher_method_call(GDBusConnection *, const gchar *sender,
                                  const gchar *, const gchar *,
                                  const gchar *method, GVariant *parameters,
                                  GDBusMethodInvocation *invocation,
                                  gpointer data) {
    auto *self = static_cast<TrayModule *>(data);

    if (std::strcmp(method, "RegisterStatusNotifierItem") == 0) {
      const gchar *service = nullptr;
      g_variant_get(parameters, "(&s)", &service);
      self->watcher_add(sender, service);
      g_dbus_method_invocation_return_value(invocation, nullptr);
    } else if (std::strcmp(method, "RegisterStatusNotifierHost") == 0) {
      self->emit_watcher_signal("StatusNotifierHostRegistered", nullptr);
      g_dbus_method_invocation_return_value(invocation, nullptr);
    } else {
      g_dbus_method_invocation_return_error(invocation, G_DBUS_ERROR,
                                            G_DBUS_ERROR_UNKNOWN_METHOD,
                                            "Unknown method %s", method);
    }
  }

  static GVariant *watcher_get_property(GDBusConnection *, const gchar *,
                                        const gchar *, const gchar *,
                                        const gchar *property, GError **,
                                        gpointer data) {
    auto *self = static_cast<TrayModule *>(data);

    if (std::strcmp(property, "RegisteredStatusNotifierItems") == 0) {
      GVariantBuilder builder;
      g_variant_builder_init(&builder, G_VARIANT_TYPE("as"));
      for (const WatchedItem *w : self->watched_)
        g_variant_builder_add(&builder, "s", w->id.c_str());
      return g_variant_builder_end(&builder);
    }

    if (std::strcmp(property, "IsStatusNotifierHostRegistered") == 0)
      return g_variant_new_boolean(TRUE);

    if (std::strcmp(property, "ProtocolVersion") == 0)
      return g_variant_new_int32(0);

    return nullptr;
  }

  static constexpr GDBusInterfaceVTable watcher_vtable_ = {
      .method_call = watcher_method_call,
      .get_property = watcher_get_property,
      .set_property = nullptr,
      .padding = {}};

  static void on_item_vanished(GDBusConnection *, const gchar *,
                               gpointer data) {
    auto *w = static_cast<WatchedItem *>(data);
    TrayModule *self = w->self;
    std::string id = w->id;
    self->watcher_remove(id);
  }

  void start_watcher() {
    GError *error = nullptr;
    node_info_ = g_dbus_node_info_new_for_xml(kWatcherXml, &error);
    if (!node_info_) {
      g_error_free(error);
      return;
    }

    watcher_object_id_ = g_dbus_connection_register_object(
        connection_, tray::kWatcherPath, node_info_->interfaces[0],
        &watcher_vtable_, this, nullptr, nullptr);

    watcher_own_id_ = g_bus_own_name_on_connection(
        connection_, tray::kWatcherName, G_BUS_NAME_OWNER_FLAGS_NONE, nullptr,
        nullptr, nullptr, nullptr);
  }

  void emit_watcher_signal(const char *name, GVariant *parameters) {
    g_dbus_connection_emit_signal(connection_, nullptr, tray::kWatcherPath,
                                  tray::kWatcherName, name, parameters,
                                  nullptr);
  }

  void watcher_add(const char *sender, const char *service) {
    std::string id;

    if (service[0] == '/')
      id = std::string(sender) + service;
    else if (std::strchr(service, '/'))
      id = service;
    else
      id = std::string(service) + "/StatusNotifierItem";

    for (const WatchedItem *w : watched_)
      if (w->id == id)
        return;

    std::string bus_name = id.substr(0, id.find('/'));

    auto *w = new WatchedItem{this, id, 0};
    w->watch_id = g_bus_watch_name_on_connection(
        connection_, bus_name.c_str(), G_BUS_NAME_WATCHER_FLAGS_NONE, nullptr,
        on_item_vanished, w, nullptr);
    watched_.push_back(w);

    emit_watcher_signal("StatusNotifierItemRegistered",
                        g_variant_new("(s)", id.c_str()));
    add_item(id);
  }

  void watcher_remove(const std::string &id) {
    for (size_t i = 0; i < watched_.size(); ++i) {
      if (watched_[i]->id != id)
        continue;

      g_bus_unwatch_name(watched_[i]->watch_id);
      delete watched_[i];
      watched_.erase(watched_.begin() + static_cast<long>(i));

      emit_watcher_signal("StatusNotifierItemUnregistered",
                          g_variant_new("(s)", id.c_str()));
      remove_item(id);
      return;
    }
  }

  static void on_watcher_appeared(GDBusConnection *, const gchar *,
                                  const gchar *, gpointer data) {
    auto *self = static_cast<TrayModule *>(data);

    g_dbus_connection_call(
        self->connection_, tray::kWatcherName, tray::kWatcherPath,
        tray::kWatcherName, "RegisterStatusNotifierHost",
        g_variant_new("(s)", self->host_name_.c_str()), nullptr,
        G_DBUS_CALL_FLAGS_NONE, -1, nullptr, nullptr, nullptr);

    g_dbus_connection_call(self->connection_, tray::kWatcherName,
                           tray::kWatcherPath,
                           "org.freedesktop.DBus.Properties", "Get",
                           g_variant_new("(ss)", tray::kWatcherName,
                                         "RegisteredStatusNotifierItems"),
                           G_VARIANT_TYPE("(v)"), G_DBUS_CALL_FLAGS_NONE, -1,
                           self->cancellable_, on_items_list, self);
  }

  static void on_watcher_vanished(GDBusConnection *, const gchar *,
                                  gpointer data) {
    auto *self = static_cast<TrayModule *>(data);

    for (auto &item : self->items_)
      self->remove(*item);
    self->items_.clear();
    self->update_visibility();
  }

  static void on_items_list(GObject *source, GAsyncResult *result,
                            gpointer data) {
    GError *error = nullptr;
    GVariant *reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source),
                                                    result, &error);

    if (!reply) {
      g_error_free(error);
      return;
    }

    auto *self = static_cast<TrayModule *>(data);

    GVariant *boxed = nullptr;
    g_variant_get(reply, "(v)", &boxed);

    GVariantIter *iter = nullptr;
    g_variant_get(boxed, "as", &iter);

    const gchar *id = nullptr;
    while (g_variant_iter_next(iter, "&s", &id))
      self->add_item(id);

    g_variant_iter_free(iter);
    g_variant_unref(boxed);
    g_variant_unref(reply);
  }

  static void on_watcher_signal(GDBusConnection *, const gchar *, const gchar *,
                                const gchar *, const gchar *signal_name,
                                GVariant *parameters, gpointer data) {
    auto *self = static_cast<TrayModule *>(data);

    const gchar *id = nullptr;

    if (std::strcmp(signal_name, "StatusNotifierItemRegistered") == 0) {
      g_variant_get(parameters, "(&s)", &id);
      self->add_item(id);
    } else if (std::strcmp(signal_name, "StatusNotifierItemUnregistered") ==
               0) {
      g_variant_get(parameters, "(&s)", &id);
      self->remove_item(id);
    }
  }

  void start_host() {
    host_name_ =
        "org.kde.StatusNotifierHost-" + std::to_string(getpid()) + "-1";

    host_own_id_ = g_bus_own_name_on_connection(
        connection_, host_name_.c_str(), G_BUS_NAME_OWNER_FLAGS_NONE, nullptr,
        nullptr, nullptr, nullptr);

    client_signal_id_ = g_dbus_connection_signal_subscribe(
        connection_, tray::kWatcherName, tray::kWatcherName, nullptr,
        tray::kWatcherPath, nullptr, G_DBUS_SIGNAL_FLAGS_NONE,
        on_watcher_signal, this, nullptr);

    watch_id_ = g_bus_watch_name_on_connection(
        connection_, tray::kWatcherName, G_BUS_NAME_WATCHER_FLAGS_NONE,
        on_watcher_appeared, on_watcher_vanished, this, nullptr);
  }

  void add_item(const std::string &id) {
    for (const auto &item : items_)
      if (item->id() == id)
        return;

    items_.push_back(std::make_unique<TrayItem>(connection_, id));
    append(*items_.back());
    update_visibility();
  }

  void remove_item(const std::string &id) {
    for (size_t i = 0; i < items_.size(); ++i) {
      if (items_[i]->id() != id)
        continue;

      remove(*items_[i]);
      items_.erase(items_.begin() + static_cast<long>(i));
      break;
    }
    update_visibility();
  }

  void update_visibility() { set_visible(!items_.empty()); }

  GDBusConnection *connection_ = nullptr;
  GCancellable *cancellable_ = nullptr;
  GDBusNodeInfo *node_info_ = nullptr;

  guint watcher_object_id_ = 0;
  guint watcher_own_id_ = 0;
  guint host_own_id_ = 0;
  guint watch_id_ = 0;
  guint client_signal_id_ = 0;

  std::string host_name_;

  std::vector<WatchedItem *> watched_;
  std::vector<std::unique_ptr<TrayItem>> items_;
};
