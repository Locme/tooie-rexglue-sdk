/**
 * @file        system/scene_hints.h
 * @brief       What the game knows about its scene, handed to the GPU's
 *              scene effects
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

namespace rex::system {

// The guest's draws don't say what they are, so a game's project tells the
// scene effects what only its game logic knows. Thread-safe.

// Whether the camera is underwater (the underwater tint, scene_fx_underwater).
void SetSceneUnderwater(bool underwater);
bool GetSceneUnderwater();

// The height of the water surface around the camera in world units (the same
// space as SetSceneCamera's rows), for the water reflections
// (scene_fx_water_reflections); ClearSceneWaterLevel when there is none.
void SetSceneWaterLevel(float height);
void ClearSceneWaterLevel();
// False when there is no water level.
bool GetSceneWaterLevel(float& height_out);
// Several water surfaces in view at different heights (pools, a lagoon and
// a river): up to kMaxSceneWaterLevels; count 0 clears.
constexpr int kMaxSceneWaterLevels = 4;
void SetSceneWaterLevels(const float* heights, int count);
// The count, heights_out gets up to kMaxSceneWaterLevels.
int GetSceneWaterLevels(float* heights_out);

}  // namespace rex::system
