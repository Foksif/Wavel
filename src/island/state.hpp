#pragma once

namespace island {

enum class State { Idle, Music, Notification, Full, Expanded };

constexpr int kMaxWidth = 480;
constexpr int kMaxHeight = 240;

constexpr float kFlare = 12.0f;
constexpr float kRadius = 16.0f;
constexpr float kCollapsedHeight = 32.0f;

constexpr float kExpandedWidth = 460.0f;
constexpr float kExpandedHeight = 240.0f;

struct Size {
  float w, h;
};

inline Size size_for(State state) {
  switch (state) {
  case State::Idle:
    return {150.0f, kCollapsedHeight};
  case State::Music:
    return {290.0f, kCollapsedHeight};
  case State::Notification:
    return {380.0f, 76.0f};
  case State::Full:
    return {440.0f, 184.0f};
  case State::Expanded:
    return {kExpandedWidth, kExpandedHeight};
  }
  return {150.0f, kCollapsedHeight};
}

} // namespace island
