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
#include <algorithm>
#include <cmath>
#include <mutex>

namespace rex::system {

namespace {

std::atomic<bool> g_underwater{false};
std::mutex g_water_mutex;
float g_water_levels[kMaxSceneWaterLevels];
int g_water_level_count = 0;

}  // namespace

void SetSceneUnderwater(bool underwater) {
  g_underwater.store(underwater, std::memory_order_relaxed);
}

bool GetSceneUnderwater() { return g_underwater.load(std::memory_order_relaxed); }

void SetSceneWaterLevel(float height) { SetSceneWaterLevels(&height, 1); }

void ClearSceneWaterLevel() { SetSceneWaterLevels(nullptr, 0); }

bool GetSceneWaterLevel(float& height_out) {
  float heights[kMaxSceneWaterLevels];
  if (!GetSceneWaterLevels(heights)) {
    return false;
  }
  height_out = heights[0];
  return true;
}

void SetSceneWaterLevels(const float* heights, int count) {
  std::lock_guard<std::mutex> lock(g_water_mutex);
  g_water_level_count = 0;
  for (int i = 0; i < count && g_water_level_count < kMaxSceneWaterLevels; ++i) {
    if (std::isfinite(heights[i])) {
      g_water_levels[g_water_level_count++] = heights[i];
    }
  }
}

int GetSceneWaterLevels(float* heights_out) {
  std::lock_guard<std::mutex> lock(g_water_mutex);
  std::copy(g_water_levels, g_water_levels + g_water_level_count, heights_out);
  return g_water_level_count;
}

}  // namespace rex::system
