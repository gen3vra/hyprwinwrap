# ![logo](logo.png) hyprwinwrap 


Display any window as a background in Hyprland.
*Need Hypr 0.54+*


---

## Installing

```sh
hyprpm add https://github.com/gen3vra/hyprwinwrap
hyprpm enable hyprwinwrap
```


## Features

- Run any program as a background, with the ability to set its position and size in percentages
- Use a dispatcher to toggle temporary focus — calling it again or changing focus resets it to a background window


## Config (Lua)

Declare background windows with `hl.plugin.hyprwinwrap.window()`. You can call it multiple times to handle multiple windows.

```lua
-- example: foot --app-id=window-bg -o colors.alpha=0.0 [path-to-script]
-- example: xterm -class window-bg [path-to-script]

-- class is an EXACT match and NOT a regex! Use `hyprctl clients` to find it.
-- You may match on `class` and/or `title`. pos_*/size_* are percentages.
if hl.plugin.hyprwinwrap ~= nil then
    hl.plugin.hyprwinwrap.window({
        class = "window-bg",
        title = "window-bg",
        layer = 0,
        pos_x = 0,
        pos_y = 10,
        size_x = 100,
        size_y = 97.5
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
| `class` | string | Window class — **exact match**, not a regex. Use `hyprctl clients` to find it. |
| `title` | string | Window title — **exact match**, not a regex. |
| `pos_x` | number | Horizontal position as a percentage of the screen width. |
| `pos_y` | number | Vertical position as a percentage of the screen height. |
| `size_x` | number | Width as a percentage of the screen width. |
| `size_y` | number | Height as a percentage of the screen height. Set to `97` to leave room for a bottom bar. |
| `layer` | number | Drawing order — higher values render on top of lower values. |

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

- Some programs check if they are hidden and may stop rendering (e.g. kitty).
- If you use an alt-tab script, make sure to skip `m_hidden` windows so they are not cycled to. Example:

```sh
previous_client="$(hyprctl clients -j | jq -r '[.[] | select(.workspace.id == '"$active_workspace"' and .hidden == false)] | sort_by(.focusHistoryID) | nth(1) | .address')"
```

---

## Examples

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

        self.pixmap = QPixmap("debian.png")

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
```

```sh
#!/bin/sh
sleep 1 && cava
```

> [!NOTE]
> The `sleep` is required for cava because window resizing happens a few milliseconds after the window opens.