---
"keyparty": minor
---

Reworked the menu into a full-screen, kiosk-native layout:

- The menu now opens full-screen — a panel anchored to the bottom with a "Click
  to start" prompt over the canvas. Clicking anywhere outside the menu starts the
  game. On short screens the menu is centered like a modal and the prompt is hidden.
- Starting collapses the menu into a thin bar at the bottom holding all the
  actions (Background, Unlock mouse, Menu, Quit) plus a Hide button. Hiding leaves
  a clean play surface; the menu chord brings the bar back.
- The grown-up chord is now a "menu chord": Control+Option+Shift+Q brings the bar
  back when hidden, and leaves the game (back to the start menu) when it's showing.
- New "Unlock mouse" action: the mouse reaches background apps while the keyboard
  stays locked to the game, with the bar itself staying clickable to re-lock.
- The panel/bar collapse, hide, and reappear with animation.
