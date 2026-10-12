/**
 * @file        system/effect_timings.cpp
 * @brief       GPU time of the scene effects, for the debug overlay
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/system/effect_timings.h>

#include <chrono>
#include <mutex>

namespace rex::system {

namespace {

std::mutex g_mutex;
std::vector<std::pair<std::string, float>> g_timings;
std::chrono::steady_clock::time_point g_time;

}  // namespace

void SetEffectTimings(std::vector<std::pair<std::string, float>> timings) {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_timings = std::move(timings);
  g_time = std::chrono::steady_clock::now();
}

std::vector<std::pair<std::string, float>> GetEffectTimings() {
  std::lock_guard<std::mutex> lock(g_mutex);
  // Stale once the effects stop (turned off, or no frames).
  if (std::chrono::steady_clock::now() - g_time > std::chrono::seconds(1)) {
    return {};
  }
  return g_timings;
}

}  // namespace rex::system
