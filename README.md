# 7aclock

Analog X11 clock with optional date display inside the clock face. Inspired by `xclock` and `urxvclock`.

![7aclock](7aclock.png)

## Features

- Lance-shaped hands (pointed tips)
- Optional date display inside the clock face
- Background transparency support (requires a compositor such as picom)
- Flicker-free rendering via double buffering
- Smart sleep: without the seconds hand the clock wakes once per minute
- Full X11 geometry support including negative offsets (e.g. `-5+5` = 5px from the right edge)
- Window title with strftime formatting (e.g. `%H:%M`)
- WM_CLASS property for window manager rules

## Building

Dependencies: `libx11`, `cairo`, `cairo-xlib`.

```sh
make
sudo make install PREFIX=/usr/local
```

## Usage

```sh
7aclock [options]
```

### Options

| Option | Description | Default |
|---|---|---|
| `-date` | Show date inside the clock face | off |
| `-dateformat FMT` | Date format string (strftime) | `%d %b` |
| `-noseconds` | Hide the seconds hand | off |
| `-noring` | Hide the outer ring of the clock face | off |
| `-alpha N` | Background opacity 0.0–1.0 | `1.0` |
| `-bg COLOR` | Background color | `#1a1a2e` |
| `-fg COLOR` | Face, ticks and border color | `#e0e0e0` |
| `-hd COLOR` | Hour and minute hand color | `#e0e0e0` |
| `-sd COLOR` | Seconds hand color | `#e05050` |
| `-dc COLOR` | Date text color | same as `-fg` |
| `-db COLOR` | Date box background | same as `-bg` |
| `-padding N` | Inner padding in pixels | `4` |
| `-title FMT` | Window title; strftime formats supported | `7aclock` |
| `-name NAME` | WM_CLASS instance name | `7aclock` |
| `-class CLASS` | WM_CLASS class name | `7aclock` |
| `-geometry WxH+X+Y` | Window geometry | — |
| `-update MS` | Fixed redraw interval in ms (overrides auto-sync) | auto |

Press `q` or `Escape` to quit.

## Examples

```sh
# Clock with date, no ring, pinned to the top-right corner
7aclock -geometry 150x150-5+5 -date -noseconds -noring \
        -fg "#7f7f7f" -hd "#a59f80" -bg grey \
        -title "%H:%M, %d %b"

# Transparent background (requires a compositor)
7aclock -alpha 0.5 -bg black -fg white

# Larger clock with a custom date format
7aclock -geometry 200x200 -date -dateformat "%A" -noseconds
```
