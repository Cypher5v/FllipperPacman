# Pac-Man for Flipper Zero

Controls: D-pad = move, OK = pause / play again, Back = exit.

## Build & install (ufbt)
```
pip install ufbt
cd pacman_game
ufbt            # builds dist/pacman_game.fap
ufbt launch     # builds, installs and runs it on a connected Flipper
```
Or copy the built `.fap` to `SD Card/apps/Games/` with qFlipper.

If you use the full firmware repo instead, put this folder in `applications_user/`
and run `./fbt fap_pacman_game`.

## Ghost behavior
Ghost 1 chases you, 2 ambushes ahead of you, 3 flanks, 4 retreats when close.
They alternate scatter/chase, and flee (hollow outlines) after a power pellet.
The row-6 side openings wrap around as a warp tunnel.
