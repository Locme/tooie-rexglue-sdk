/**
 * @file        system/scene_camera.cpp
 * @brief       A camera the game hands to the GPU's scene effects
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/system/scene_camera.h>

#include <cstring>
#include <mutex>

namespace rex::system {

namespace {

std::mutex g_scene_camera_mutex;
float g_scene_camera_rows[4][4];
bool g_scene_camera_valid = false;

}  // namespace

void SetSceneCamera(const float rows[4][4]) {
  std::lock_guard<std::mutex> lock(g_scene_camera_mutex);
  std::memcpy(g_scene_camera_rows, rows, sizeof(g_scene_camera_rows));
  g_scene_camera_valid = true;
}

void ClearSceneCamera() {
  std::lock_guard<std::mutex> lock(g_scene_camera_mutex);
  g_scene_camera_valid = false;
}

bool GetSceneCamera(float rows_out[4][4]) {
  std::lock_guard<std::mutex> lock(g_scene_camera_mutex);
  if (!g_scene_camera_valid) {
    return false;
  }
  std::memcpy(rows_out, g_scene_camera_rows, sizeof(g_scene_camera_rows));
  return true;
}

}  // namespace rex::system
