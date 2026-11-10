#pragma once

#include <cmath>

namespace island {

struct Spring {
  float value = 0.0f;
  float velocity = 0.0f;
  float target = 0.0f;

  void step(float dt, float stiffness, float damping) {
    float accel = stiffness * (target - value) - damping * velocity;
    velocity += accel * dt;
    value += velocity * dt;
  }

  bool settled() const {
    return std::fabs(target - value) < 0.05f && std::fabs(velocity) < 0.5f;
  }
};

} // namespace island
