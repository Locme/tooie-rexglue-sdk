/**
 * @file        system/scene_camera.h
 * @brief       A camera the game hands to the GPU's scene effects
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

namespace rex::system {

// Games that transform their vertices on the CPU (N64-style ports) never put
// a view-projection in the vertex shader constants, so the scene effects
// can't find the camera there (scene_fx_camera_constant). Such a game's
// project calls SetSceneCamera from a hook on its camera update instead,
// with the world-to-clip rows the way the constants would hold them:
//   clip = rows * (x, y, z, 1); rows[3] is the w row (the camera's unit
//   forward and -forward.position), rows[2] = a * rows[3] + (0, 0, 0, b)
//   maps view distance to stored depth d = a + b / distance.
// The GPU plugin reads it with GetSceneCamera for every depth-writing draw
// until ClearSceneCamera. Thread-safe.
void SetSceneCamera(const float rows[4][4]);
void ClearSceneCamera();
// False when no camera was set (rows left untouched).
bool GetSceneCamera(float rows_out[4][4]);

}  // namespace rex::system
