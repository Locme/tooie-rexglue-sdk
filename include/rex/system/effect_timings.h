/**
 * @file        system/effect_timings.h
 * @brief       GPU time of the scene effects, for the debug overlay
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <string>
#include <utility>
#include <vector>

namespace rex::system {

// The GPU plugin's scene effects publish how long each of their passes took
// on the GPU (milliseconds per frame, smoothed; scene_fx_timing), the debug
// overlay (F3) shows them. Thread-safe.
void SetEffectTimings(std::vector<std::pair<std::string, float>> timings);
// Empty when nothing was measured recently.
std::vector<std::pair<std::string, float>> GetEffectTimings();

}  // namespace rex::system
