#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

#include <pipewire/context.h>
#include <pipewire/extensions/metadata.h>
#include <pipewire/node.h>
#include <pipewire/pipewire.h>
#include <pipewire/thread-loop.h>

#include <spa/param/props.h>
#include <spa/pod/builder.h>
#include <spa/pod/iter.h>
#include <spa/utils/hook.h>
#include <spa/utils/type.h>

class AudioManager {
public:
  AudioManager() { init(); }

  ~AudioManager() {
    if (!loop_)
      return;

    pw_thread_loop_lock(loop_);

    unbind_endpoint(output_);
    unbind_endpoint(input_);

    spa_hook_remove(&metadata_listener_);
    if (metadata_)
      pw_proxy_destroy(reinterpret_cast<pw_proxy *>(metadata_));

    spa_hook_remove(&registry_listener_);
    if (registry_)
      pw_proxy_destroy(reinterpret_cast<pw_proxy *>(registry_));
    if (core_)
      pw_core_disconnect(core_);

    pw_thread_loop_unlock(loop_);

    pw_thread_loop_stop(loop_);

    if (context_)
      pw_context_destroy(context_);

    pw_thread_loop_destroy(loop_);

    pw_deinit();
  }

  AudioManager(const AudioManager &) = delete;
  AudioManager &operator=(const AudioManager &) = delete;

  float output_volume() const { return get_volume(output_); }
  float input_volume() const { return get_volume(input_); }
  bool output_muted() const { return get_muted(output_); }
  bool input_muted() const { return get_muted(input_); }

  void set_output_volume(float volume) { set_volume(output_, volume); }
  void set_input_volume(float volume) { set_volume(input_, volume); }
  void set_output_muted(bool muted) { set_muted(output_, muted); }
  void set_input_muted(bool muted) { set_muted(input_, muted); }

private:
  struct Endpoint {
    pw_node *node = nullptr;
    uint32_t id = SPA_ID_INVALID;
    spa_hook listener{};
    float volume = 1.0f;
    bool muted = false;
    uint32_t channels = 2;
  };

  struct NodeInfo {
    uint32_t id;
    std::string name;
    bool is_sink;
  };

  void init() {
    pw_init(nullptr, nullptr);

    loop_ = pw_thread_loop_new("wavel-audio", nullptr);
    if (!loop_)
      throw std::runtime_error("Failed to create PipeWire thread loop");

    context_ = pw_context_new(pw_thread_loop_get_loop(loop_), nullptr, 0);
    if (!context_) {
      pw_thread_loop_destroy(loop_);
      loop_ = nullptr;
      throw std::runtime_error("Failed to create PipeWire context");
    }

    pw_thread_loop_lock(loop_);

    if (pw_thread_loop_start(loop_) < 0) {
      pw_thread_loop_unlock(loop_);
      pw_context_destroy(context_);
      pw_thread_loop_destroy(loop_);
      context_ = nullptr;
      loop_ = nullptr;
      throw std::runtime_error("Failed to start PipeWire thread loop");
    }

    core_ = pw_context_connect(context_, nullptr, 0);
    if (!core_) {
      pw_thread_loop_unlock(loop_);
      pw_thread_loop_stop(loop_);
      pw_context_destroy(context_);
      pw_thread_loop_destroy(loop_);
      context_ = nullptr;
      loop_ = nullptr;
      throw std::runtime_error("Failed to connect to PipeWire");
    }

    registry_ = pw_core_get_registry(core_, PW_VERSION_REGISTRY, 0);
    if (!registry_) {
      pw_core_disconnect(core_);
      pw_thread_loop_unlock(loop_);
      pw_thread_loop_stop(loop_);
      pw_context_destroy(context_);
      pw_thread_loop_destroy(loop_);
      core_ = nullptr;
      context_ = nullptr;
      loop_ = nullptr;
      throw std::runtime_error("Failed to get PipeWire registry");
    }

    pw_registry_add_listener(registry_, &registry_listener_, &registry_events_,
                             this);

    pw_thread_loop_unlock(loop_);
  }

  static void read_props(Endpoint &ep, const spa_pod *param) {
    auto *object =
        reinterpret_cast<spa_pod_object *>(const_cast<spa_pod *>(param));

    spa_pod_prop *prop;

    SPA_POD_OBJECT_FOREACH(object, prop) {
      if (prop->key == SPA_PROP_channelVolumes) {
        if (SPA_POD_TYPE(&prop->value) != SPA_TYPE_Array ||
            SPA_POD_ARRAY_VALUE_TYPE(&prop->value) != SPA_TYPE_Float)
          continue;

        uint32_t n = 0;
        const float *values =
            static_cast<const float *>(spa_pod_get_array(&prop->value, &n));

        if (!values || n == 0)
          continue;

        float max_linear = 0.0f;
        for (uint32_t i = 0; i < n; ++i)
          max_linear = std::max(max_linear, values[i]);

        ep.channels = n;
        ep.volume = std::cbrt(max_linear);
      }

      if (prop->key == SPA_PROP_mute) {
        bool muted = false;
        if (spa_pod_get_bool(&prop->value, &muted) >= 0)
          ep.muted = muted;
      }
    }
  }

  static void on_param(void *data, int, uint32_t id, uint32_t, uint32_t,
                       const spa_pod *param) {
    auto *ep = static_cast<Endpoint *>(data);

    if (id != SPA_PARAM_Props || !param ||
        SPA_POD_TYPE(param) != SPA_TYPE_Object)
      return;

    read_props(*ep, param);
  }

  static constexpr pw_node_events node_events_ = {
      .version = PW_VERSION_NODE_EVENTS, .info = nullptr, .param = on_param};

  void unbind_endpoint(Endpoint &ep) {
    if (ep.node) {
      spa_hook_remove(&ep.listener);
      ep.listener = {};
      pw_proxy_destroy(reinterpret_cast<pw_proxy *>(ep.node));
      ep.node = nullptr;
    }
    ep.id = SPA_ID_INVALID;
  }

  void bind_endpoint(Endpoint &ep, uint32_t id) {
    unbind_endpoint(ep);

    ep.node = static_cast<pw_node *>(pw_registry_bind(
        registry_, id, PW_TYPE_INTERFACE_Node, PW_VERSION_NODE, 0));

    if (!ep.node)
      return;

    ep.id = id;

    pw_node_add_listener(ep.node, &ep.listener, &node_events_, &ep);

    uint32_t ids[] = {SPA_PARAM_Props};
    pw_node_subscribe_params(ep.node, ids, 1);
    pw_node_enum_params(ep.node, 0, SPA_PARAM_Props, 0, UINT32_MAX, nullptr);
  }

  void select_endpoint(Endpoint &ep, const std::string &default_name,
                       bool is_sink) {
    if (default_name.empty())
      return;

    for (const auto &n : nodes_) {
      if (n.is_sink == is_sink && n.name == default_name) {
        if (ep.id != n.id)
          bind_endpoint(ep, n.id);
        return;
      }
    }
  }

  void apply_defaults() {
    select_endpoint(output_, default_sink_, true);
    select_endpoint(input_, default_source_, false);
  }

  static std::string parse_name(const char *value) {
    if (!value)
      return {};

    std::string s(value);
    const std::string key = "\"name\"";

    size_t pos = s.find(key);
    if (pos == std::string::npos)
      return {};

    pos = s.find('"', s.find(':', pos + key.size()) + 1);
    if (pos == std::string::npos)
      return {};

    size_t end = s.find('"', pos + 1);
    if (end == std::string::npos)
      return {};

    return s.substr(pos + 1, end - pos - 1);
  }

  static int on_metadata_property(void *data, uint32_t, const char *key,
                                  const char *, const char *value) {
    auto *self = static_cast<AudioManager *>(data);

    if (!key)
      return 0;

    if (std::strcmp(key, "default.audio.sink") == 0) {
      self->default_sink_ = parse_name(value);
      self->apply_defaults();
    } else if (std::strcmp(key, "default.audio.source") == 0) {
      self->default_source_ = parse_name(value);
      self->apply_defaults();
    }

    return 0;
  }

  static constexpr pw_metadata_events metadata_events_ = {
      .version = PW_VERSION_METADATA_EVENTS, .property = on_metadata_property};

  static void on_global(void *data, uint32_t id, uint32_t, const char *type,
                        uint32_t, const spa_dict *props) {
    auto *self = static_cast<AudioManager *>(data);

    if (!type || !props)
      return;

    if (std::strcmp(type, PW_TYPE_INTERFACE_Metadata) == 0) {
      const char *name = spa_dict_lookup(props, "metadata.name");

      if (name && std::strcmp(name, "default") == 0 && !self->metadata_) {
        self->metadata_ = static_cast<pw_metadata *>(
            pw_registry_bind(self->registry_, id, PW_TYPE_INTERFACE_Metadata,
                             PW_VERSION_METADATA, 0));

        if (self->metadata_)
          pw_metadata_add_listener(self->metadata_, &self->metadata_listener_,
                                   &metadata_events_, self);
      }
      return;
    }

    if (std::strcmp(type, PW_TYPE_INTERFACE_Node) != 0)
      return;

    const char *media_class = spa_dict_lookup(props, "media.class");
    const char *node_name = spa_dict_lookup(props, "node.name");

    if (!media_class || !node_name)
      return;

    bool is_sink = std::strcmp(media_class, "Audio/Sink") == 0;
    bool is_source = std::strcmp(media_class, "Audio/Source") == 0;

    if (!is_sink && !is_source)
      return;

    self->nodes_.push_back({id, node_name, is_sink});
    self->apply_defaults();
  }

  static void on_global_remove(void *data, uint32_t id) {
    auto *self = static_cast<AudioManager *>(data);

    self->nodes_.erase(
        std::remove_if(self->nodes_.begin(), self->nodes_.end(),
                       [id](const NodeInfo &n) { return n.id == id; }),
        self->nodes_.end());

    if (self->output_.id == id)
      self->unbind_endpoint(self->output_);
    if (self->input_.id == id)
      self->unbind_endpoint(self->input_);
  }

  static constexpr pw_registry_events registry_events_ = {
      .version = PW_VERSION_REGISTRY_EVENTS,
      .global = on_global,
      .global_remove = on_global_remove};

  float get_volume(const Endpoint &ep) const {
    pw_thread_loop_lock(loop_);
    float value = ep.volume;
    pw_thread_loop_unlock(loop_);
    return value;
  }

  bool get_muted(const Endpoint &ep) const {
    pw_thread_loop_lock(loop_);
    bool value = ep.muted;
    pw_thread_loop_unlock(loop_);
    return value;
  }

  void set_volume(Endpoint &ep, float volume) {
    volume = std::clamp(volume, 0.0f, 1.0f);

    pw_thread_loop_lock(loop_);

    if (ep.node) {
      ep.volume = volume;

      const float linear = volume * volume * volume;
      const uint32_t channels = std::clamp<uint32_t>(ep.channels, 1, 32);
      std::vector<float> values(channels, linear);

      uint8_t buffer[1024];
      spa_pod_builder builder;
      spa_pod_builder_init(&builder, buffer, sizeof(buffer));

      const spa_pod *props =
          static_cast<const spa_pod *>(spa_pod_builder_add_object(
              &builder, SPA_TYPE_OBJECT_Props, SPA_PARAM_Props,
              SPA_PROP_channelVolumes,
              SPA_POD_Array(sizeof(float), SPA_TYPE_Float, channels,
                            values.data())));

      pw_node_set_param(ep.node, SPA_PARAM_Props, 0, props);
    }

    pw_thread_loop_unlock(loop_);
  }

  void set_muted(Endpoint &ep, bool muted) {
    pw_thread_loop_lock(loop_);

    if (ep.node) {
      ep.muted = muted;

      uint8_t buffer[256];
      spa_pod_builder builder;
      spa_pod_builder_init(&builder, buffer, sizeof(buffer));

      const spa_pod *props =
          static_cast<const spa_pod *>(spa_pod_builder_add_object(
              &builder, SPA_TYPE_OBJECT_Props, SPA_PARAM_Props, SPA_PROP_mute,
              SPA_POD_Bool(muted)));

      pw_node_set_param(ep.node, SPA_PARAM_Props, 0, props);
    }

    pw_thread_loop_unlock(loop_);
  }

  pw_thread_loop *loop_ = nullptr;
  pw_context *context_ = nullptr;
  pw_core *core_ = nullptr;
  pw_registry *registry_ = nullptr;
  pw_metadata *metadata_ = nullptr;

  spa_hook registry_listener_{};
  spa_hook metadata_listener_{};

  std::vector<NodeInfo> nodes_;
  std::string default_sink_;
  std::string default_source_;

  Endpoint output_;
  Endpoint input_;
};
