# The GPU-composited session — INSTALLED, 2026-08-22

These three files make sway the autostarted desktop, replacing LXQt-on-X. They
are live on the device.

- `gemini-sway.desktop` -> `/usr/share/wayland-sessions/`
- `gemini-session`      -> `/usr/local/bin/`  (sets WLR_RENDERER=gles2 and
                            WLR_RENDER_DRM_DEVICE; NOT gemini-dock, which stops
                            sddm and is the manual-start version)
- `sway-docked.conf`    -> `/etc/gemini/`

Point sddm at it in **two** places, because it reads both:

    /etc/sddm.conf.d/10-gemini.conf   Session=gemini-sway.desktop
    /var/lib/sddm/state.conf          Session=/usr/share/wayland-sessions/gemini-sway.desktop

Two traps, both of which cost time on the day this landed:

1. **sddm reads every file in `/etc/sddm.conf.d/`, whatever it is called.** A
   backup left there as `10-gemini.conf.lxqt-backup` sorts *after*
   `10-gemini.conf` and silently overrode it, so sddm kept starting LXQt while
   the config said sway. Keep backups somewhere else.
2. **`sway-docked.conf` used to contain nothing but two `output` lines.** Sway
   replaces its whole default config when given `-c`, so that session had no
   keybindings at all — no terminal, no window controls, no bar. This version
   `include`s `/etc/sway/config` first and then overrides.

Reverting is the same two `Session=` lines set back to `lxqt.desktop` and
`/usr/share/xsessions/lxqt.desktop`.

## Verifying it, without fooling yourself

`grim` proves only that the compositor rendered. Photograph the panel — and
**pin the camera exposure**, which `gemini-eyes.py` now does by default. On
auto-exposure this camera blows a lit panel in a dark room to a uniform pale
cyan, and a black screen and a white screen come out identical. That is how a
perfectly working sway desktop was recorded as a frozen panel for an hour
(B-46, issue #58, both retracted).

Known gaps, neither blocking:

- `$mod+d` does nothing: the stock config's `$menu` is `wmenu-run`, which is
  not installed. `$mod+Return` gives a terminal.
- The LXQt panel cannot run natively — `qt6-wayland` is not installed and the
  device needs to be online to get it. LXQt *applications* run fine through
  Xwayland and get Mali-T880 (Panfrost), not llvmpipe.
