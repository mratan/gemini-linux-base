# The GPU-composited session — staged, and BLOCKED

These are the three files that make sway the autostarted desktop. They work:
sddm reads `gemini-sway.desktop`, runs `gemini-session`, sway comes up as the
`gemini` user on the active VT with DRM master, swaybar and a terminal run, and
`grim` captures a perfect 2160x1080 desktop.

**They are not installed, because the frames never reach the glass.** See B-46.
The panel keeps showing a stale buffer while the compositor renders correctly.
Setting the background to `#000000` and then `#ffffff` produces two photographs
that are pixel-for-pixel the same cyan rectangle.

Do not install these until B-46 is fixed. Reverting is
`Session=lxqt.desktop` in `/etc/sddm.conf.d/10-gemini.conf` **and** in
`/var/lib/sddm/state.conf` — sddm reads every file in `sddm.conf.d`, including
one called `10-gemini.conf.lxqt-backup`, so do not leave backups in there.

`sway-docked.conf` here is worth keeping regardless: the version on the device
contained nothing but two `output` lines, which means the session had no
keybindings at all — no terminal, no window controls, no bar. This one includes
`/etc/sway/config` first.
