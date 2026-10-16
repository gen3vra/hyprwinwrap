# ![logo](logo.png) hyprwinwrap 


Display any window as a background in Hyprland.
*Need Hypr 0.54+*


---

## Installing

```sh
hyprpm add https://github.com/gen3vra/hyprwinwrap
hyprpm enable hyprwinwrap
```

## Config (Lua)

Declare background windows with `hl.plugin.hyprwinwrap.window()`. You can call it multiple times to handle multiple windows.

```lua
-- example: foot --app-id=window-bg -o colors.alpha=0.0 [path-to-script]
-- example: kitty --class=window-bg -o background_opacity=0.0 [path-to-script]
-- example: xterm -class window-bg [path-to-script]
-- any program will work, use `hyprctl clients` to discover your window's class/title

-- class is an EXACT match and NOT a regex! Use `hyprctl clients` to find it.
-- You may match on `class` and/or `title`. pos_*/size_* are percentages.
if hl.plugin.hyprwinwrap ~= nil then
    hl.plugin.hyprwinwrap.window({
        class = "window-bg",
        title = "window-bg",
        layer = 0,
        pos_x = 0,
        pos_y = 0,
        size_x = 100,
        size_y = 97
    })
    -- Second bg window sitting in the centre on top of the first,
    -- useful for showing a visualizer only on a portion of the screen.
    hl.plugin.hyprwinwrap.window({
        class = "window-bg2",
        title = "window-bg2",
        layer = 1,
        pos_x = 25,
        pos_y = 25,
        size_x = 50,
        size_y = 50
    })
end
```

### Fields

| Field | Type | Description |
|-------|------|-------------|
| `class` | string | Window class **exact match**, not a regex. Use `hyprctl clients` to find it. |
| `title` | string | Window title **exact match**, not a regex. |
| `pos_x` | number | Horizontal position as a percentage of the screen width. |
| `pos_y` | number | Vertical position as a percentage of the screen height. |
| `size_x` | number | Width as a percentage of the screen width. Any Lua expression works, like `200 / (1 + math.sqrt(5))` for a golden ratio window :D (61.8% ish) |
| `size_y` | number | Height as a percentage of the screen height. `100 * 1000 / 1080` gives exactly 1000px on a 1080p screen. |
| `layer` | number | Higher values render on top of lower values. |
| `monitor` | string | Monitor name or selector (`DP-1`, `desc:...`). Omit to keep the monitor the window opened on. |
| `opacity` | number | 0 to 1. Wins over any window rule. See [Opacity](#opacity). |

### Opacity

Background windows do **not** follow Hyprland's normal opacity: global rules, non-override rules and `decoration:inactive_opacity` are ignored so a blanket `opacity = "1.0 0.8"` never dims a Hyprwinwrap window. With no `opacity` key and no override rule the window renders fully opaque. Two things apply, in this order:

1. `opacity = 0.5` on the `window({...})` spec.
2. A window rule with `override`:

```lua
hl.window_rule({
    match = {class = "window-bg2"},
    opacity = "0.5 override"
})
```

```lua
hl.plugin.hyprwinwrap.window({
    class = "window-bg2",
    title = "window-bg2",
    layer = 1,
    pos_x = 25,
    pos_y = 25,
    size_x = 50,
    size_y = 50,
    opacity = 0.5
})
```

The same opacity stays in effect while the window is toggled interactive.

### Focus Dispatcher

`hl.plugin.hyprwinwrap.focus("[title-or-class]")` toggles focus on the specified window. Calling it again or changing focus resets it to the background.

Bind it in your config:

```lua
hl.bind("SUPER + B", function() hl.plugin.hyprwinwrap.focus("window-bg") end)
```

Or call it directly via `hyprctl`:

```sh
hyprctl dispatch 'hl.plugin.hyprwinwrap.focus("window-bg")'
```


## Config (hyprlang, deprecated)

> [!WARNING]
> Hyprland is dropping hyprlang support in an upcoming release. This method is legacy and does **not** support multiple windows or layers. This will be removed in a future version.

```ini
plugin {
    # example: foot --app-id=window-bg -o colors.alpha=0.0 [path-to-script]
    # example: xterm -class window-bg [path-to-script]
    hyprwinwrap {
        # class is an EXACT match and NOT a regex!
        class = window-bg  # use hyprctl clients to find the class of your window
        # you can also match on title
        title = window-bg
        # position as a percentage of the screen
        pos_x = 0
        pos_y = 0
        # size as a percentage of the screen
        size_x = 100
        size_y = 97  # 100 would cover a bottom waybar; 97 leaves space for it
    }
}
```

Lua config equivalent (for example only, use the [lua config](#config-lua) method instead):

```lua
hl.config({ plugin = { hyprwinwrap = {
    class = "window-bg", title = "window-bg",
    pos_x = 0, pos_y = 10, size_x = 100, size_y = 97,
} } })
```

### Focus Dispatcher

```sh
hyprctl dispatch hyprwinwrap_interactivity
```

---

## Notes

- If you use an alt-tab script (or are designing around hyprwinwrap), make sure to skip `m_hidden` windows so they are not cycled to. Example:

```sh
previous_client="$(hyprctl clients -j | jq -r '[.[] | select(.workspace.id == '"$active_workspace"' and .hidden == false)] | sort_by(.focusHistoryID) | nth(1) | .address')"
```

---

## Examples

### Fitting Above A Bar

Run `hyprctl layers` to see your bar's y position (its `xywh`), then set `size_y = 100 * y / screen_height`. On a 1080p screen with a waybar at y=1050, that's `100 * 1050 / 1080`

### Transparent Image

Displays over `window-bg` using `window-bg2` from the Lua config example above.

Install PyQt6:

```sh
pip install PyQt6
```

```python
import sys
from PyQt6.QtWidgets import QApplication, QLabel
from PyQt6.QtGui import QPixmap
from PyQt6.QtCore import Qt


class hi(QLabel):
    def __init__(self):
        super().__init__()

        self.pixmap = QPixmap("image.png") # assuming img next to script

        self.setWindowFlags(
            Qt.WindowType.FramelessWindowHint
            | Qt.WindowType.WindowStaysOnTopHint
        )
        self.setAttribute(Qt.WidgetAttribute.WA_TranslucentBackground)

        self.setAlignment(Qt.AlignmentFlag.AlignCenter)

        self.resize(800, 600)

    def resizeEvent(self, event):
        self.setPixmap(
            self.pixmap.scaled(
                self.size(),
                Qt.AspectRatioMode.KeepAspectRatio,  # OR IgnoreAspectRatio to stretch
                Qt.TransformationMode.SmoothTransformation,
            )
        )


app = QApplication(sys.argv)
app.setApplicationName("window-bg2")
app.setDesktopFileName("window-bg2")

window = hi()
window.show()

sys.exit(app.exec())
```

### Cava

Launch via a terminal emulator, for example:

```sh
foot --app-id=window-bg -o colors-[dark|light].alpha=0.0 cava.sh
kitty --class=window-bg -o background_opacity=0.0 cava.sh
```

```sh
#!/bin/sh
sleep 1 && cava
```

> [!NOTE]
> The `sleep` is required for cava because window resizing happens a few milliseconds after the window opens and cava will be at the wrong size.
