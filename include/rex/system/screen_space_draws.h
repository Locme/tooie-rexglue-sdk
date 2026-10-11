/**
 * @file        system/screen_space_draws.h
 * @brief       A horizontal scale the game asks for its 2D (HUD) draws
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

namespace rex::system {

// A game that widens its camera to a wider window than it was made for still
// draws its HUD for the original aspect, so the presented image stretches it.
// Its project calls SetScreenSpaceDrawScale with original aspect / window
// aspect: the GPU plugin then narrows every draw it recognises as
// screen-space 2D (see rex/graphics/pipeline/screen_space_draws.h) by that
// factor about the screen's center. 1 (the default) turns it off.
// Thread-safe.
void SetScreenSpaceDrawScale(float x_scale);
float GetScreenSpaceDrawScale();

}  // namespace rex::system
