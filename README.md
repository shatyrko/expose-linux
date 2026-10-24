# Expose Linux

macOS Exposé / Mission Control for Wayland: every window of every desktop as a live thumbnail over the blurred desktop,
as big as they can be while they all fit. Built for the [Umbriel](https://docs.noctalia.dev/umbriel/) compositor, with
GTK4 + gtk4-layer-shell for the overlay and a small C helper for the window pictures.

- **Keyboard first**: arrows move, Enter goes to the window, Esc closes, Tab / Shift+Tab step, Delete closes the selected
  window, typing letters picks windows by title or app name (the others dim). The pointer hides when you use the keyboard.
- **Mouse**: click a card to go to that window, middle-click (or the x on a card) to close it, click the background to leave.
- **Live pictures** of every window, also those on other desktops or scrolled out of view, taken with
  `ext-image-copy-capture` (`tools/expose-shots.c`); the last pictures show at once while fresh ones arrive.
- Colours from the active Noctalia palette, font from GTK, blur from an Umbriel layer rule (below).
- Runs in the background (no tray icon); `expose-toggle` opens it in milliseconds.

## Install

Needs `python-gobject`, `gtk4`, `gtk4-layer-shell`, and to build the helper `gcc`, `wayland`, `wayland-protocols`.

```sh
git clone https://github.com/shatyrko/expose-linux && cd expose-linux
bash install.sh
```

Then add to `~/.config/umbriel/config.toml`:

```toml
[[layer_rule]]
match.namespace = "^expose-linux$"
blur = true
blur_ignore_alpha = 0.1
blur_optimized = false

[keybinds]
"Mod+Alt+Up" = { action = "spawn:expose-toggle", repeat = false }
```

It uses `umbriel windows --json` for the window list and the actions `window-focus:<id>` / `window-close:<id>`
(the window `id` is the same string as the ext-foreign-toplevel identifier). Other compositors that offer the same
capture protocols and a way to focus / close a window by id could be supported by changing `umbriel_windows()` and
`umbriel_msg()` in `expose-linux`.

`expose-linux --list` prints the windows it would show.
