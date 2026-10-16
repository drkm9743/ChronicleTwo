#pragma once

// Draws the banner naming the character a quick change (L3, R3) switched to, over the game frame.
void PortDrawQuickChangeBanner();

// Drops a banner still showing, as a mode change and its loading screen begin.
void PortCancelQuickChangeBanner();

// Plays the effect on the character a quick change switched to: a fade from white and a splash of stars.
void PortStepQuickChangeEffect();

// Whether the last quick change is still playing its effect; no new quick change starts until it ends.
bool PortQuickChangeCoolingDown();

// Completes a quick change the frame just opened, in that same frame, so the game never stops for its menu.
void PortFinishQuickChange();
