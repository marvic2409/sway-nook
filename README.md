# Sway Nook

A small, native shelf for Sway’s scratchpad. Tuck windows away, see their saved previews in a translucent bottom shelf, and bring them back by mouse or keyboard. Previews stay opaque. The shelf starts on demand and listens for Sway events while open.

![Two example scratchpad cards](docs/assets/sway-nook.png)

## Install

Requires Sway, Grim, GTK 3, GTK Layer Shell, JSON-GLib, Meson, and a C compiler. On Arch Linux:

```sh
sudo pacman -S --needed sway grim gtk3 gtk-layer-shell json-glib meson ninja base-devel
meson setup build --prefix="$HOME/.local" --buildtype=release
meson compile -C build
meson install -C build
```

Add `~/.local/bin` to the environment Sway inherits, then include [`config/sway-nook.conf`](config/sway-nook.conf) in your Sway config. The default bindings are **Super+Shift+−** to stash the focused window and **Super+−** to toggle the shelf. Reload Sway’s config after adding them.

Click a card or press **Enter** to restore it. Use **Left/Right**, **Tab**, or **h/j/k/l** to move between cards; **Esc** closes the shelf. Grouped windows appear as one card and restore together.

```sh
sway-nook show          # open without toggling off
sway-nook list          # hidden windows as JSON
sway-nook restore 123   # restore by container ID
```

## Theme

Copy [`config/theme.ini`](config/theme.ini) to `~/.config/sway-nook/theme.ini` and edit its colors (`#RRGGBB`) and opacities (`0`–`1`). Changes apply the next time the shelf opens. The defaults are black, white, and gray. The panel and cards can be translucent; preview pixels and their backing stay opaque.

Snapshots live only under your private `$XDG_RUNTIME_DIR/sway-nook-<uid>/` directory. They are removed when the corresponding window closes or the login session ends. If Grim cannot capture a window, its card shows a placeholder.

## Arch package

Build and install the Arch package with `makepkg -si` from `packaging/arch/`.

For development: `meson test -C build --print-errorlogs`. The code is GPL-3.0-only.
