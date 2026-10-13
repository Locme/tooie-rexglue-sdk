/**
 * @file        system/scene_hints.cpp
 * @brief       What the game knows about its scene, handed to the GPU's
 *              scene effects
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/system/scene_hints.h>

#include <atomic>
#include <cmath>

namespace rex::system {

namespace {

std::atomic<bool> g_underwater{false};
// NaN = no water level.
std::atomic<float> g_water_level{NAN};

}  // namespace

void SetSceneUnderwater(bool underwater) {
  g_underwater.store(underwater, std::memory_order_relaxed);
}

bool GetSceneUnderwater() { return g_underwater.load(std::memory_order_relaxed); }

void SetSceneWaterLevel(float height) { g_water_level.store(height, std::memory_order_relaxed); }

void ClearSceneWaterLevel() { g_water_level.store(NAN, std::memory_order_relaxed); }

bool GetSceneWaterLevel(float& height_out) {
  float height = g_water_level.load(std::memory_order_relaxed);
  if (std::isnan(height)) {
    return false;
  }
  height_out = height;
  return true;
}

}  // namespace rex::system
