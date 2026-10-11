/**
 * @file        system/screen_space_draws.cpp
 * @brief       A horizontal scale the game asks for its 2D (HUD) draws
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/system/screen_space_draws.h>

#include <atomic>

namespace rex::system {

namespace {

std::atomic<float> g_screen_space_draw_scale{1.0f};

}  // namespace

void SetScreenSpaceDrawScale(float x_scale) {
  g_screen_space_draw_scale.store(x_scale, std::memory_order_relaxed);
}

float GetScreenSpaceDrawScale() {
  return g_screen_space_draw_scale.load(std::memory_order_relaxed);
}

}  // namespace rex::system
