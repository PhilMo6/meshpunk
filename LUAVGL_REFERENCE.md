# LuaVGL Complete Reference

Lua bindings for LVGL exposed via the `lvgl` global. All UI in Meshpunk apps is built with these bindings.

```lua
local root = lvgl.Object()
root:set { w = lvgl.HOR_RES(), h = lvgl.VER_RES() }
local label = root:Label { text = "Hello", align = lvgl.ALIGN.CENTER }
```

T-Deck display: 320x240, RGB565.

---

## Core Concepts

**Object hierarchy** — Everything descends from Object. Widgets are created as children: `parent:Label{...}`. If no parent is given, the object is created on the LVGL root screen.

**Property system** — Properties can be set three ways:
- Constructor table: `root:Label { text = "Hi", w = 100 }`
- Set method: `obj:set { text = "Hi", w = 100 }`
- Style with selector: `obj:set_style({ bg_color = "#FF0000" }, lvgl.PART.MAIN + lvgl.STATE.PRESSED)`

**Color formats** — `0xFF0000` (hex int), `"#FF0000"` or `"#F00"` (hex string), or palette result.

**Opacity** — `lvgl.OPA(100)` converts 0-100 to LVGL's 0-255 range.

**Sizing helpers** — `lvgl.PCT(50)` = 50% of parent, `lvgl.SIZE_CONTENT` = fit to content, `lvgl.HOR_RES()` / `lvgl.VER_RES()` = screen dimensions.

**Memory** — Lua GC owns objects created from Lua. Call `:delete()` to explicitly destroy an object and its children. Style objects also need manual `:delete()`.

---

## Object (Base Widget)

All widgets inherit these methods and properties.

**Constructor:** `lvgl.Object([parent], [props])` or `parent:Object([props])`

### Direct Properties (get/set)

| Property | Type | Description |
|----------|------|-------------|
| `w` | int | Width |
| `h` | int | Height |
| `align` | table/int | Alignment (see Alignment section) |
| `id` | string | String identifier for `get_child_by_id()` |
| `user_data` | any | Arbitrary Lua value stored on the object |

### Methods

**Lifecycle:**
| Method | Signature | Description |
|--------|-----------|-------------|
| `set` | `obj:set(props_table)` | Set multiple properties at once |
| `get` | `obj:get(prop_name)` | Get a property value |
| `delete` | `obj:delete()` | Delete object and all children |
| `clean` | `obj:clean()` | Delete all children |
| `invalidate` | `obj:invalidate()` | Force redraw |

**Hierarchy:**
| Method | Signature | Description |
|--------|-----------|-------------|
| `set_parent` | `obj:set_parent(parent)` | Move to new parent |
| `get_parent` | `obj:get_parent()` | Returns parent object |
| `get_child` | `obj:get_child(index)` | Get child (0 = first, -1 = last) |
| `get_child_cnt` | `obj:get_child_cnt()` | Number of children |
| `get_child_by_id` | `obj:get_child_by_id(id_str)` | Find child by string id |
| `get_screen` | `obj:get_screen()` | Get the screen this object is on |
| `move_to_index` | `obj:move_to_index(idx)` | Reorder among siblings |

**Positioning:**
| Method | Signature | Description |
|--------|-----------|-------------|
| `center` | `obj:center()` | Center on parent |
| `align_to` | `obj:align_to({base=obj, type=ALIGN, x_ofs=0, y_ofs=0})` | Align relative to another object |
| `get_coords` | `obj:get_coords()` | Returns `{x1, y1, x2, y2}` in screen coords |
| `get_pos` | `obj:get_pos()` | Returns `{x1, y1, x2, y2}` relative to parent |

**Flags & State:**
| Method | Signature | Description |
|--------|-----------|-------------|
| `add_flag` | `obj:add_flag(FLAG)` | Add a flag (e.g. `lvgl.FLAG.CLICKABLE`) |
| `clear_flag` | `obj:clear_flag(FLAG)` | Remove a flag |
| `add_state` | `obj:add_state(STATE)` | Add a state (e.g. `lvgl.STATE.FOCUSED`) |
| `clear_state` | `obj:clear_state(STATE)` | Remove a state |
| `get_state` | `obj:get_state()` | Get current state bitmask |
| `is_visible` | `obj:is_visible()` | Returns boolean |
| `is_editable` | `obj:is_editable()` | Returns boolean |
| `is_group_def` | `obj:is_group_def()` | Returns boolean |

**Scrolling:**
| Method | Signature | Description |
|--------|-----------|-------------|
| `scroll_to` | `obj:scroll_to({x=0, y=0, anim=true})` | Scroll to absolute position |
| `scroll_by` | `obj:scroll_by(dx, dy, [anim])` | Scroll by delta |
| `scroll_by_bounded` | `obj:scroll_by_bounded(dx, dy, anim)` | Scroll by delta with bounds |
| `scroll_by_raw` | `obj:scroll_by_raw(x, y, anim)` | Low-level scroll |
| `scroll_to_view` | `obj:scroll_to_view([anim])` | Scroll until visible on parent |
| `scroll_to_view_recursive` | `obj:scroll_to_view_recursive([anim])` | Scroll into view through nested parents |
| `is_scrolling` | `obj:is_scrolling()` | Returns boolean |
| `get_scroll_top` | `obj:get_scroll_top()` | Current scroll offset |
| `scrollbar_invalidate` | `obj:scrollbar_invalidate()` | Redraw scrollbars |
| `readjust_scroll` | `obj:readjust_scroll(anim)` | Snap content back if overscrolled |

**Layout:**
| Method | Signature | Description |
|--------|-----------|-------------|
| `set_flex_flow` | `obj:set_flex_flow(FLEX_FLOW)` | Set flex direction/wrap |
| `set_flex_align` | `obj:set_flex_align(main, cross, track)` | Set flex alignment |
| `set_flex_grow` | `obj:set_flex_grow(value)` | Set flex grow factor (0-255) |
| `is_layout_positioned` | `obj:is_layout_positioned()` | Check if positioned by layout |
| `mark_layout_as_dirty` | `obj:mark_layout_as_dirty()` | Force layout recalculation |

**Style:**
| Method | Signature | Description |
|--------|-----------|-------------|
| `set_style` | `obj:set_style(props, [selector])` | Set style with optional part+state selector |
| `add_style` | `obj:add_style(style_obj, [selector])` | Apply a reusable Style object |
| `remove_style` | `obj:remove_style(style_obj, [selector])` | Remove a style |
| `remove_style_all` | `obj:remove_style_all()` | Remove all added styles |

**Events:**
| Method | Signature | Description |
|--------|-----------|-------------|
| `onevent` | `obj:onevent(EVENT_CODE, callback)` | Register event handler |
| `onPressed` | `obj:onPressed(callback)` | Shortcut for EVENT.PRESSED |
| `onClicked` | `obj:onClicked(callback)` | Shortcut for EVENT.CLICKED |
| `onShortClicked` | `obj:onShortClicked(callback)` | Shortcut for EVENT.SHORT_CLICKED |

Callback signature: `function(obj, event_code)`. One handler per event code per object — setting again replaces the previous. Pass `nil` to remove.

**Animation:**
| Method | Signature | Description |
|--------|-----------|-------------|
| `Anim` / `anim` | `obj:Anim(params)` | Create animation (see Animation section) |
| `remove_all_anim` | `obj:remove_all_anim()` | Stop and remove all animations |

**Misc:**
| Method | Signature | Description |
|--------|-----------|-------------|
| `indev_search` | `obj:indev_search(point)` | Find object at screen coordinates |

### Example

```lua
local container = lvgl.Object()
container:set {
    w = lvgl.HOR_RES(), h = lvgl.VER_RES(),
    bg_color = "#000000", bg_opa = lvgl.OPA(100),
    flex = {
        flex_direction = "column",
        justify_content = "center",
        align_items = "center",
    }
}

local btn = container:Object {
    w = 200, h = 50,
    bg_color = "#2196F3", bg_opa = lvgl.OPA(100),
    radius = 8,
}
btn:add_flag(lvgl.FLAG.CLICKABLE)
btn:onClicked(function(obj, code)
    print("Button clicked!")
end)
```

---

## Widget Reference

### Button

Styled Object wrapper. Typically contains a Label child for text.

**Constructor:** `lvgl.Button([parent], [props])` or `parent:Button([props])`

No widget-specific methods beyond Object base.

```lua
local btn = root:Button { w = 120, h = 40, radius = 5 }
btn:Label { text = "Click Me", align = lvgl.ALIGN.CENTER }
btn:onClicked(function() print("clicked") end)
```

---

### Label

Text display widget.

**Constructor:** `lvgl.Label([parent], [props])` or `parent:Label([props])`

**Properties (via constructor or `:set{}`):**

| Property | Type | Description |
|----------|------|-------------|
| `text` | string | Display text |
| `long_mode` | int | Text overflow: `lvgl.LABEL_CONST.LONG_WRAP`, `LONG_DOT`, `LONG_SCROLL`, `LONG_SCROLL_CIRCULAR`, `LONG_CLIP` |

**Methods:**

| Method | Signature | Description |
|--------|-----------|-------------|
| `get_text` | `label:get_text()` | Returns current text string |
| `get_long_mode` | `label:get_long_mode()` | Returns current long mode |
| `get_recolor` | `label:get_recolor()` | Returns recolor attribute |
| `ins_text` | `label:ins_text(pos, str)` | Insert text at position |
| `cut_text` | `label:cut_text(pos, count)` | Delete characters at position |

`__tostring` returns the label text.

```lua
local lbl = root:Label {
    text = "Score: 0",
    text_color = "#FFFFFF",
    text_font = lvgl.Font("montserrat", 18),
    align = lvgl.ALIGN.TOP_MID,
}
lbl:set { text = "Score: 42" }
lbl:ins_text(7, "1") -- "Score: 142"
```

---

### Image

Image display with transform support.

**Constructor:** `lvgl.Image([parent], [props])` or `parent:Image([props])`

**Properties (via constructor or `:set{}`):**

| Property | Type | Description |
|----------|------|-------------|
| `src` | string | Image file path |
| `offset_x` | int | Image offset X |
| `offset_y` | int | Image offset Y |
| `angle` | int | Rotation in 0.1 degree units (3600 = 360 degrees) |
| `zoom` | int | Zoom level (256 = 100%, `lvgl.LV_ZOOM_NONE`) |
| `antialias` | bool | Enable antialiasing |
| `pivot` | table | Rotation pivot `{x, y}` |

**Methods:**

| Method | Signature | Description |
|--------|-----------|-------------|
| `set_src` | `img:set_src(path)` | Change image source |
| `set_offset` | `img:set_offset({x=0, y=0})` | Set image offset |
| `set_pivot` | `img:set_pivot({x=0, y=0})` | Set rotation pivot point |
| `get_img_size` | `img:get_img_size([src])` | Returns `w, h` of current or specified image |

```lua
local icon = root:Image {
    src = "P/icons/settings.png",
    align = lvgl.ALIGN.CENTER,
}
local w, h = icon:get_img_size()

-- Rotate image
icon:set { angle = 900, pivot = { x = w/2, y = h/2 } }
```

---

### Checkbox

Boolean toggle with text label.

**Constructor:** `lvgl.Checkbox([parent], [props])` or `parent:Checkbox([props])`

**Properties:**

| Property | Type | Description |
|----------|------|-------------|
| `text` | string | Checkbox label text |

**Methods:**

| Method | Signature | Description |
|--------|-----------|-------------|
| `get_text` | `cb:get_text()` | Returns label text |

```lua
local cb = root:Checkbox { text = "Enable notifications" }
cb:onevent(lvgl.EVENT.VALUE_CHANGED, function(obj)
    -- check state with obj:get_state()
    local checked = (obj:get_state() & lvgl.STATE.CHECKED) ~= 0
    print("Checked:", checked)
end)
```

---

### Dropdown

Select widget with expandable option list.

**Constructor:** `lvgl.Dropdown([parent], [props])` or `parent:Dropdown([props])`

**Properties:**

| Property | Type | Description |
|----------|------|-------------|
| `options` | string | Newline-separated options: `"Option 1\nOption 2\nOption 3"` |
| `selected` | int | Selected option index (0-based) |
| `dir` | DIR | Direction to open: `lvgl.DIR.BOTTOM`, `TOP`, `LEFT`, `RIGHT` |
| `symbol` | string/ptr | Arrow symbol |
| `highlight` | bool | Highlight selected option |

**Methods:**

| Method | Signature | Description |
|--------|-----------|-------------|
| `open` | `dd:open()` | Open the dropdown list |
| `close` | `dd:close()` | Close the dropdown list |
| `is_open` | `dd:is_open()` | Returns boolean |
| `add_option` | `dd:add_option(text, pos)` | Add option at position |
| `clear_option` | `dd:clear_option()` | Remove all options |
| `get` | `dd:get(which, [arg])` | Get property: `"list"`, `"options"`, `"selected"`, `"option_cnt"`, `"selected_str"`, `"option_index"`, `"symbol"`, `"dir"` |

```lua
local dd = root:Dropdown {
    options = "Low\nMedium\nHigh",
    selected = 1,
    w = 150,
}
dd:onevent(lvgl.EVENT.VALUE_CHANGED, function(obj)
    local sel = dd:get("selected_str")
    print("Selected:", sel)
end)
dd:add_option("Ultra", lvgl.DROPDOWN_POS_LAST)
```

---

### Calendar

Date picker widget.

**Constructor:** `lvgl.Calendar([parent], [props])` or `parent:Calendar([props])`

**Properties:**

| Property | Type | Description |
|----------|------|-------------|
| `today` | table | `{year=2026, month=5, day=11}` |
| `showed` | table | `{year=2026, month=5}` — displayed month |

**Methods:**

| Method | Signature | Description |
|--------|-----------|-------------|
| `get_today` | `cal:get_today()` | Returns `{year, month, day}` |
| `get_showed` | `cal:get_showed()` | Returns `{year, month, day}` |
| `get_pressed` | `cal:get_pressed()` | Returns pressed date `{year, month, day}` |
| `get_btnm` | `cal:get_btnm()` | Get the button matrix child object |
| `Arrow` | `cal:Arrow()` | Create arrow navigation header |
| `Dropdown` | `cal:Dropdown()` | Create dropdown navigation header |

```lua
local cal = root:Calendar {
    today = { year = 2026, month = 5, day = 11 },
    showed = { year = 2026, month = 5 },
    w = 250, h = 250,
}
cal:Arrow() -- adds < > month navigation
cal:onevent(lvgl.EVENT.VALUE_CHANGED, function(obj)
    local date = obj:get_pressed()
    print(date.year, date.month, date.day)
end)
```

---

### Canvas

Pixel-level drawing surface with shape primitives.

**Constructor:** `lvgl.Canvas([parent], {w=width, h=height, [cf=COLOR_FORMAT]})` or `parent:Canvas({w=, h=, [cf=]})`

Color format defaults to RGB565. See `lvgl.COLOR_FORMAT.*` for options.

**Pixel Methods:**

| Method | Signature | Description |
|--------|-----------|-------------|
| `fill_bg` | `canvas:fill_bg(color, opa)` | Fill entire canvas |
| `set_px` | `canvas:set_px(x, y, color, opa)` | Set single pixel |
| `get_px` | `canvas:get_px(x, y)` | Returns `{r, g, b, a}` |
| `set_palette` | `canvas:set_palette(index, color32)` | Set palette entry (indexed formats) |

**Drawing Methods:**

Each draw method takes a parameter table. All fields are optional except where noted.

#### `draw_rect`
```lua
canvas:draw_rect {
    x1 = 10, y1 = 10, x2 = 100, y2 = 50,  -- required: bounds
    radius = 5,
    bg_color = "#2196F3",   bg_opa = lvgl.OPA(100),
    border_color = "#FFF",  border_width = 2,  border_opa = lvgl.OPA(100),
    shadow_color = "#000",  shadow_width = 10, shadow_opa = lvgl.OPA(50),
    shadow_offset_x = 2,   shadow_offset_y = 2, shadow_spread = 0,
    outline_color = "#FFF", outline_width = 1, outline_opa = lvgl.OPA(100), outline_pad = 0,
}
```

#### `draw_line`
```lua
canvas:draw_line {
    p1 = { x = 10, y = 10 },  -- required
    p2 = { x = 100, y = 50 }, -- required
    color = "#FFFFFF", width = 2, opa = lvgl.OPA(100),
    dash_width = 0, dash_gap = 0,
    round_start = false, round_end = false,
}
```

#### `draw_arc`
```lua
canvas:draw_arc {
    center = { x = 50, y = 50 }, -- required
    radius = 40,                  -- required
    start_angle = 0, end_angle = 270,
    color = "#FF0000", width = 3, opa = lvgl.OPA(100),
    rounded = false,
}
```

#### `draw_label`
```lua
canvas:draw_label {
    text = "Hello",                    -- required
    x1 = 10, y1 = 10, x2 = 200, y2 = 50,
    color = "#FFFFFF", opa = lvgl.OPA(100),
    font = lvgl.Font("montserrat", 14),
    letter_space = 0, line_space = 0,
}
```

#### `draw_triangle`
```lua
canvas:draw_triangle {
    p1 = { x = 50, y = 10 },   -- required
    p2 = { x = 10, y = 90 },   -- required
    p3 = { x = 90, y = 90 },   -- required
    bg_color = "#FF0000", bg_opa = lvgl.OPA(100),
}
```

#### `draw_image`
```lua
canvas:draw_image {
    src = "P/image.png",              -- required
    x1 = 0, y1 = 0, x2 = 100, y2 = 100,
    opa = lvgl.OPA(100),
    rotation = 0,          -- 0.1 degree units
    scale_x = 256, scale_y = 256, -- 256 = 100%
    pivot = { x = 50, y = 50 },
}
```

### Canvas Example

```lua
local canvas = root:Canvas { w = 200, h = 150 }
canvas:fill_bg("#000000", lvgl.OPA(100))
canvas:draw_rect {
    x1 = 10, y1 = 10, x2 = 190, y2 = 140,
    radius = 10, bg_color = "#1a1a2e", bg_opa = lvgl.OPA(100),
    border_color = "#e94560", border_width = 2, border_opa = lvgl.OPA(100),
}
canvas:draw_line {
    p1 = { x = 10, y = 75 }, p2 = { x = 190, y = 75 },
    color = "#e94560", width = 1, opa = lvgl.OPA(50),
}
canvas:draw_label {
    text = "Canvas!", x1 = 60, y1 = 60, x2 = 190, y2 = 90,
    color = "#FFFFFF", font = lvgl.Font("montserrat", 18),
}
```

---

### Keyboard

Virtual on-screen keyboard. Typically paired with a Textarea.

**Constructor:** `lvgl.Keyboard([parent], [props])` or `parent:Keyboard([props])`

**Properties:**

| Property | Type | Description |
|----------|------|-------------|
| `textarea` | Textarea | Target textarea object to receive input |
| `mode` | KEYBOARD_MODE | Layout: `TEXT_LOWER`, `TEXT_UPPER`, `SPECIAL`, `NUMBER`, `USER_1`-`USER_4` |
| `popovers` | bool | Show button titles in popover on press |

```lua
local ta = root:Textarea {
    w = 280, h = 40, one_line = true,
    placeholder = "Type here...",
}
local kb = root:Keyboard {
    textarea = ta,
    mode = lvgl.KEYBOARD_MODE.TEXT_LOWER,
    w = lvgl.HOR_RES(), h = 120,
    align = lvgl.ALIGN.BOTTOM_MID,
}
```

---

### Led

LED indicator widget with brightness control.

**Constructor:** `lvgl.Led([parent], [props])` or `parent:Led([props])`

**Properties:**

| Property | Type | Description |
|----------|------|-------------|
| `color` | color | LED color |
| `brightness` | int | Brightness 0-255 |

**Methods:**

| Method | Signature | Description |
|--------|-----------|-------------|
| `on` | `led:on()` | Turn on (max brightness) |
| `off` | `led:off()` | Turn off (min brightness) |
| `toggle` | `led:toggle()` | Toggle on/off |
| `get_brightness` | `led:get_brightness()` | Returns current brightness |

```lua
local led = root:Led {
    color = "#00FF00", brightness = 200,
    w = 30, h = 30, align = lvgl.ALIGN.CENTER,
}
led:on()
-- Blink with timer
lvgl.Timer {
    period = 500,
    cb = function(t) led:toggle() end,
}
```

---

### List

Scrollable list with text items and buttons.

**Constructor:** `lvgl.List([parent], [props])` or `parent:List([props])`

**Methods:**

| Method | Signature | Description |
|--------|-----------|-------------|
| `add_text` | `list:add_text(str)` | Add a text-only item, returns Label |
| `add_btn` | `list:add_btn(icon_src, text)` | Add a button item (icon can be nil), returns Object |
| `get_btn_text` | `list:get_btn_text(btn)` | Get text of a button item |

```lua
local list = root:List { w = 200, h = 200, align = lvgl.ALIGN.CENTER }
list:add_text("Settings")
local btn1 = list:add_btn(nil, "WiFi")
local btn2 = list:add_btn(nil, "Bluetooth")
local btn3 = list:add_btn(nil, "Display")

btn1:onClicked(function()
    print("WiFi selected")
end)
```

---

### Roller

Spinning drum selector.

**Constructor:** `lvgl.Roller([parent], [props])` or `parent:Roller([props])`

**Properties:**

| Property | Type | Description |
|----------|------|-------------|
| `options` | string or table | `"Jan\nFeb\nMar"` or `{options="Jan\nFeb\nMar", mode=ROLLER_MODE}` |
| `selected` | int or table | Index or `{selected=2, anim=true}` |
| `visible_cnt` | int | Number of visible rows |

**Methods:**

| Method | Signature | Description |
|--------|-----------|-------------|
| `get_options` | `roller:get_options()` | Returns options string |
| `get_selected` | `roller:get_selected()` | Returns selected index |
| `get_selected_str` | `roller:get_selected_str()` | Returns selected option text |
| `get_options_cnt` | `roller:get_options_cnt()` | Returns total option count |

```lua
local roller = root:Roller {
    options = {
        options = "Jan\nFeb\nMar\nApr\nMay\nJun\nJul\nAug\nSep\nOct\nNov\nDec",
        mode = lvgl.ROLLER_MODE.INFINITE,
    },
    visible_cnt = 3,
    selected = { selected = 4, anim = true },
    w = 100,
}
roller:onevent(lvgl.EVENT.VALUE_CHANGED, function(obj)
    print("Month:", obj:get_selected_str())
end)
```

---

### Textarea

Text input with cursor, selection, and password support.

**Constructor:** `lvgl.Textarea([parent], [props])` or `parent:Textarea([props])`

**Properties:**

| Property | Type | Description |
|----------|------|-------------|
| `text` | string | Current text content |
| `placeholder` | string | Placeholder text shown when empty |
| `one_line` | bool | Single-line mode (no line breaks) |
| `password_mode` | bool | Hide characters with bullets |
| `password_bullet` | string | Custom bullet character |
| `password_show_time` | int | Ms to show character before hiding |
| `accepted_chars` | string | Whitelist of allowed characters, e.g. `"0123456789"` |
| `max_length` | int | Maximum text length |
| `cursor` | int | Cursor position (`lvgl.TEXTAREA_CURSOR_LAST` for end) |

**Methods:**

| Method | Signature | Description |
|--------|-----------|-------------|
| `get_text` | `ta:get_text()` | Returns current text |

```lua
local ta = root:Textarea {
    w = 250, h = 80,
    placeholder = "Enter message...",
    max_length = 200,
}

-- Single-line password field
local pw = root:Textarea {
    w = 250, h = 40,
    one_line = true,
    password_mode = true,
    placeholder = "Password",
}

-- Numeric-only input
local num = root:Textarea {
    w = 100, h = 40,
    one_line = true,
    accepted_chars = "0123456789.-",
}
```

---

## Style System

### Reusable Style Objects

```lua
local style = lvgl.Style {
    bg_color = "#333333",
    bg_opa = lvgl.OPA(100),
    radius = 10,
    border_color = "#666666",
    border_width = 1,
}

-- Apply to objects
obj1:add_style(style)
obj2:add_style(style)

-- Apply only when pressed
obj1:add_style(style, lvgl.PART.MAIN + lvgl.STATE.PRESSED)

-- Update the style (affects all objects using it)
style:set { bg_color = "#444444" }

-- Remove from object
obj1:remove_style(style)

-- Clean up
style:delete()
```

**Style Methods:**

| Method | Signature | Description |
|--------|-----------|-------------|
| `set` | `style:set(props)` | Update style properties |
| `delete` | `style:delete()` | Delete style (required for GC) |
| `remove_prop` | `style:remove_prop("prop_name")` | Remove a specific property |

### Style Selectors

Selectors combine `PART` + `STATE` with addition:

```lua
-- Style the scrollbar when pressed
obj:set_style({ bg_color = "#FF0000" }, lvgl.PART.SCROLLBAR + lvgl.STATE.PRESSED)

-- Style the indicator part
obj:set_style({ bg_color = "#00FF00" }, lvgl.PART.INDICATOR)
```

### Style Property Reference

#### Dimensions & Position

| Property | Type | Description |
|----------|------|-------------|
| `width` | int | Width in pixels |
| `min_width` | int | Minimum width |
| `max_width` | int | Maximum width |
| `height` | int | Height in pixels |
| `min_height` | int | Minimum height |
| `max_height` | int | Maximum height |
| `x` | int | X position |
| `y` | int | Y position |
| `align` | int | Alignment constant |
| `size` | int | Shorthand: sets both width and height |

#### Transform

| Property | Type | Description |
|----------|------|-------------|
| `transform_width` | int | Extra width added for drawing |
| `transform_height` | int | Extra height added for drawing |
| `translate_x` | int | X translation offset |
| `translate_y` | int | Y translation offset |
| `transform_scale_x` | int | X scale (256 = 100%) |
| `transform_scale_y` | int | Y scale (256 = 100%) |
| `transform_rotation` | int | Rotation in 0.1 degree units |
| `transform_pivot_x` | int | Rotation pivot X |
| `transform_pivot_y` | int | Rotation pivot Y |

#### Padding

| Property | Type | Description |
|----------|------|-------------|
| `pad_top` | int | Top padding |
| `pad_bottom` | int | Bottom padding |
| `pad_left` | int | Left padding |
| `pad_right` | int | Right padding |
| `pad_row` | int | Row gap (between children in column layout) |
| `pad_column` | int | Column gap (between children in row layout) |
| `pad_gap` | int | Gap between children |
| `pad_all` | int | Shorthand: all four sides |
| `pad_ver` | int | Shorthand: top + bottom |
| `pad_hor` | int | Shorthand: left + right |

#### Background

| Property | Type | Description |
|----------|------|-------------|
| `bg_color` | color | Background color |
| `bg_opa` | int | Background opacity (0-255 or use `OPA()`) |
| `bg_grad_color` | color | Gradient end color |
| `bg_grad_dir` | int | Gradient direction |
| `bg_main_stop` | int | Gradient main color stop (0-255) |
| `bg_grad_stop` | int | Gradient end color stop (0-255) |
| `bg_grad` | table | Complex gradient descriptor |
| `bg_image_src` | string | Background image path |
| `bg_image_opa` | int | Background image opacity |
| `bg_image_recolor` | color | Background image recolor |
| `bg_image_recolor_opa` | int | Background image recolor opacity |
| `bg_image_tiled` | int | Tile the background image |

#### Border

| Property | Type | Description |
|----------|------|-------------|
| `border_color` | color | Border color |
| `border_opa` | int | Border opacity |
| `border_width` | int | Border width in pixels |
| `border_side` | int | Which sides to draw border on |
| `border_post` | int | Draw border after children |

#### Outline

| Property | Type | Description |
|----------|------|-------------|
| `outline_width` | int | Outline width |
| `outline_color` | color | Outline color |
| `outline_opa` | int | Outline opacity |
| `outline_pad` | int | Gap between outline and object |

#### Shadow

| Property | Type | Description |
|----------|------|-------------|
| `shadow_width` | int | Shadow blur radius |
| `shadow_offset_x` | int | Shadow X offset |
| `shadow_offset_y` | int | Shadow Y offset |
| `shadow_spread` | int | Shadow spread |
| `shadow_color` | color | Shadow color |
| `shadow_opa` | int | Shadow opacity |

#### Text

| Property | Type | Description |
|----------|------|-------------|
| `text_color` | color | Text color |
| `text_opa` | int | Text opacity |
| `text_font` | Font | Font (from `lvgl.Font()` or `BUILTIN_FONT.*`) |
| `text_letter_space` | int | Extra space between letters |
| `text_line_space` | int | Extra space between lines |
| `text_decor` | int | Text decoration (underline, strikethrough) |
| `text_align` | int | Text alignment within label |

#### Image

| Property | Type | Description |
|----------|------|-------------|
| `image_opa` | int | Image opacity |
| `image_recolor` | color | Image recolor tint |
| `image_recolor_opa` | int | Image recolor intensity |

#### Line

| Property | Type | Description |
|----------|------|-------------|
| `line_width` | int | Line width |
| `line_dash_width` | int | Dash segment length |
| `line_dash_gap` | int | Gap between dashes |
| `line_rounded` | int | Round line endings |
| `line_color` | color | Line color |
| `line_opa` | int | Line opacity |

#### Arc

| Property | Type | Description |
|----------|------|-------------|
| `arc_width` | int | Arc stroke width |
| `arc_image_src` | string | Image source for arc |
| `arc_rounded` | int | Round arc endpoints |
| `arc_color` | color | Arc color |
| `arc_opa` | int | Arc opacity |

#### Appearance

| Property | Type | Description |
|----------|------|-------------|
| `radius` | int | Corner radius (`lvgl.RADIUS_CIRCLE` for fully round) |
| `clip_corner` | int | Clip children to rounded corners |
| `opa` | int | Overall object opacity |
| `color_filter_opa` | int | Color filter opacity |
| `color_filter_dsc` | special | Color filter descriptor |
| `anim_time` | int | Default animation time in ms |
| `blend_mode` | int | Blend mode |
| `layout` | int | Layout type (`lvgl.LAYOUT_FLEX` or `lvgl.LAYOUT_GRID`) |
| `base_dir` | int | Base direction (LTR/RTL) |
| `transition` | special | Transition descriptor for smooth property changes |

#### Flex Layout (via style)

| Property | Type | Description |
|----------|------|-------------|
| `flex` | table | CSS-like shorthand (see Layout section) |
| `flex_flow` | int | Flow constant from `FLEX_FLOW.*` |
| `flex_main_place` | int | Main axis alignment from `FLEX_ALIGN.*` |
| `flex_cross_place` | int | Cross axis alignment from `FLEX_ALIGN.*` |
| `flex_track_place` | int | Multi-line track alignment from `FLEX_ALIGN.*` |
| `flex_grow` | int | Grow factor 0-255 |

**Special value:** Set any property to `"inherit"` to inherit from parent.

---

## Animation System

Animate any numeric property over time with easing.

**Constructor:** `obj:Anim(params)` or `obj:anim(params)`

### Parameters

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `run` | bool | false | Start immediately |
| `start_value` | int | — | Starting value |
| `end_value` | int | — | Ending value |
| `duration` | int | — | Duration in milliseconds |
| `delay` | int | 0 | Delay before start in ms |
| `repeat_count` | int | 1 | Repeats (0 = no repeat, `ANIM_REPEAT_INFINITE` = forever) |
| `repeat_delay` | int | 0 | Delay between repeats in ms |
| `early_apply` | bool | true | Apply start_value immediately |
| `playback_time` | int | 0 | Duration of reverse playback (0 = no playback) |
| `playback_delay` | int | 0 | Delay before playback in ms |
| `path` | string | `"linear"` | Easing function |
| `exec_cb` | function | — | Called each frame: `function(obj, value)` |
| `done_cb` | function | — | Called when finished: `function(anim, obj)` |

### Easing Paths

| Path | Description |
|------|-------------|
| `"linear"` | Constant speed |
| `"ease_in"` | Slow start, fast end |
| `"ease_out"` | Fast start, slow end |
| `"ease_in_out"` | Slow start and end |
| `"overshoot"` | Goes past end value then settles |
| `"bounce"` | Bounces at the end |
| `"step"` | Instant jump to end value |

### Animation Methods

| Method | Signature | Description |
|--------|-----------|-------------|
| `start` | `anim:start()` | Start (if not auto-started with `run`) |
| `stop` | `anim:stop()` | Pause animation |
| `set` | `anim:set(params)` | Update parameters |
| `delete` | `anim:delete()` | Remove animation |

### Example

```lua
-- Fade in
obj:Anim {
    run = true,
    start_value = 0,
    end_value = 255,
    duration = 500,
    path = "ease_out",
    exec_cb = function(obj, value)
        obj:set { opa = value }
    end,
}

-- Continuous rotation
local img = root:Image { src = "P/spinner.png" }
img:Anim {
    run = true,
    start_value = 0,
    end_value = 3600,
    duration = 2000,
    repeat_count = lvgl.ANIM_REPEAT_INFINITE,
    path = "linear",
    exec_cb = function(obj, value)
        obj:set { angle = value }
    end,
}

-- Bounce with playback
obj:Anim {
    run = true,
    start_value = 0,
    end_value = 50,
    duration = 300,
    playback_time = 300,
    repeat_count = lvgl.ANIM_REPEAT_INFINITE,
    path = "ease_in_out",
    exec_cb = function(obj, value)
        obj:set { translate_y = value }
    end,
}
```

---

## Timer System

Periodic or one-shot callbacks.

**Constructor:** `lvgl.Timer(params)`

### Parameters

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `period` | int | — | Interval in milliseconds |
| `repeat_count` | int | -1 | Number of times to fire (-1 = infinite) |
| `cb` | function | — | Callback: `function(timer)` |
| `paused` | bool | false | Start paused |

### Timer Methods

| Method | Signature | Description |
|--------|-----------|-------------|
| `set` | `timer:set(params)` | Update parameters |
| `pause` | `timer:pause()` | Pause timer |
| `resume` | `timer:resume()` | Resume timer |
| `delete` | `timer:delete()` | Stop and remove timer |
| `ready` | `timer:ready()` | Fire callback on next loop iteration |

### Example

```lua
-- Update clock every second
local clock_timer = lvgl.Timer {
    period = 1000,
    cb = function(t)
        clock_label:set { text = os.date("%H:%M:%S") }
    end,
}

-- One-shot delay
lvgl.Timer {
    period = 3000,
    repeat_count = 1,
    cb = function(t)
        t:delete()
        splash_screen:delete()
    end,
}
```

---

## Font System

**Constructor:** `lvgl.Font(family, size, [weight])`

| Parameter | Type | Description |
|-----------|------|-------------|
| `family` | string | Font family name. Comma-separated for fallback: `"MiSans, montserrat"` |
| `size` | int | Font size in pixels |
| `weight` | string/int/nil | Weight (default: `"normal"` / 400) |

### Weight Values

| Name | Value |
|------|-------|
| `"thin"` | 100 |
| `"extra light"` | 200 |
| `"light"` | 300 |
| `"normal"` | 400 |
| `"medium"` | 500 |
| `"semi bold"` | 600 |
| `"bold"` | 700 |
| `"extra bold"` | 800 |
| `"ultra bold"` | 900 |

### Builtin Font Constants

Use directly without `Font()` constructor:

```lua
label:set { text_font = lvgl.BUILTIN_FONT.MONTSERRAT_14 }
```

Available: `DEFAULT`, `MONTSERRAT_8` through `MONTSERRAT_48` (even sizes), `MONTSERRAT_28_COMPRESSED`, `DEJAVU_16_PERSIAN_HEBREW`, `SIMSUN_16_CJK`, `UNSCII_8`, `UNSCII_16`.

Note: Which fonts are available depends on compile-time `lv_conf.h` settings.

```lua
-- Using Font constructor
local font_large = lvgl.Font("montserrat", 28, "normal")
label:set { text_font = font_large }

-- Fallback chain
local font = lvgl.Font("MiSans medium, montserrat", 16)
```

---

## Event System

### Registering Events

```lua
-- Generic event handler
obj:onevent(lvgl.EVENT.VALUE_CHANGED, function(obj, code)
    print("Value changed!")
end)

-- Convenience shortcuts
obj:onPressed(function(obj, code) end)
obj:onClicked(function(obj, code) end)
obj:onShortClicked(function(obj, code) end)

-- Remove handler
obj:onevent(lvgl.EVENT.CLICKED, nil)
```

**Rules:**
- One handler per event code per object. Setting again replaces the previous.
- Callback signature: `function(obj, event_code)`
- Use `lvgl.EVENT.ALL` to catch every event on an object.

---

## Layout (Flex)

### CSS-Style Shorthand

The most ergonomic way to use flex layout:

```lua
container:set {
    flex = {
        flex_direction = "row",        -- "row", "column", "row-reverse", "column-reverse"
        flex_wrap = "wrap",            -- "nowrap", "wrap", "wrap-reverse"
        justify_content = "center",    -- "flex-start", "flex-end", "center",
                                       -- "space-between", "space-around", "space-evenly"
        align_items = "center",        -- same options as justify_content
        align_content = "center",      -- same options as justify_content
    }
}
```

### Direct Methods

```lua
obj:set_flex_flow(lvgl.FLEX_FLOW.ROW_WRAP)
obj:set_flex_align(
    lvgl.FLEX_ALIGN.CENTER,        -- main axis (justify)
    lvgl.FLEX_ALIGN.CENTER,        -- cross axis (align-items)
    lvgl.FLEX_ALIGN.CENTER         -- track (align-content)
)
child:set_flex_grow(1) -- this child expands to fill available space
```

### Via Style Properties

```lua
obj:set {
    flex_flow = lvgl.FLEX_FLOW.COLUMN,
    flex_main_place = lvgl.FLEX_ALIGN.SPACE_BETWEEN,
    flex_cross_place = lvgl.FLEX_ALIGN.CENTER,
}
```

### Sizing Helpers

| Function/Constant | Description |
|--------------------|-------------|
| `lvgl.PCT(50)` | 50% of parent dimension |
| `lvgl.SIZE_CONTENT` | Fit to content |
| `lvgl.HOR_RES()` | Screen width (320 on T-Deck) |
| `lvgl.VER_RES()` | Screen height (240 on T-Deck) |
| `lvgl.COORD_MAX` | Maximum coordinate value |
| `lvgl.COORD_MIN` | Minimum coordinate value |

### Layout Example

```lua
local page = lvgl.Object()
page:set {
    w = lvgl.HOR_RES(), h = lvgl.VER_RES(),
    flex = { flex_direction = "column" },
    pad_all = 8, pad_row = 4,
    bg_color = "#000", bg_opa = lvgl.OPA(100),
}

-- Header fills width
local header = page:Object {
    w = lvgl.PCT(100), h = 30,
    bg_color = "#1a1a2e", bg_opa = lvgl.OPA(100),
}
header:Label { text = "My App", text_color = "#FFF", align = lvgl.ALIGN.CENTER }

-- Content area expands to fill remaining space
local content = page:Object {
    w = lvgl.PCT(100), h = lvgl.SIZE_CONTENT,
    flex = { flex_direction = "row", flex_wrap = "wrap", justify_content = "space-evenly" },
    pad_all = 4, pad_row = 4, pad_column = 4,
}
content:set_flex_grow(1)

for i = 1, 6 do
    local card = content:Object {
        w = 90, h = 60,
        bg_color = "#16213e", bg_opa = lvgl.OPA(100),
        radius = 6,
    }
    card:Label { text = "Item " .. i, align = lvgl.ALIGN.CENTER, text_color = "#FFF" }
end
```

---

## Utility Modules

### Display (`lvgl.disp`)

Screen management and transitions.

| Function | Signature | Description |
|----------|-----------|-------------|
| `get_default` | `lvgl.disp.get_default()` | Get default display |
| `get_scr_act` | `lvgl.disp.get_scr_act([disp])` | Get active screen |
| `get_scr_prev` | `lvgl.disp.get_scr_prev([disp])` | Get previous screen |
| `get_next` | `lvgl.disp.get_next([disp])` | Get next display |
| `load_scr` | `lvgl.disp.load_scr(obj, [params])` | Load screen with transition |

**`load_scr` params:** `{anim=SCR_LOAD_ANIM, time=ms, delay=ms, auto_del=bool}`

**Display instance methods:**

| Method | Description |
|--------|-------------|
| `disp:get_layer_top()` | Get top layer |
| `disp:get_layer_sys()` | Get system layer |
| `disp:set_rotation(deg)` | Set display rotation |
| `disp:get_res()` | Returns `w, h` |

```lua
local new_screen = lvgl.Object()
new_screen:set { w = lvgl.HOR_RES(), h = lvgl.VER_RES(), bg_color = "#000" }

lvgl.disp.load_scr(new_screen, {
    anim = lvgl.SCR_LOAD_ANIM.FADE_ON,
    time = 300,
    delay = 0,
    auto_del = true,
})
```

---

### Input Device (`lvgl.indev`)

Read pointer position, gestures, and keyboard input.

| Function | Signature | Description |
|----------|-----------|-------------|
| `get_act` | `lvgl.indev.get_act()` | Get active input device |
| `get_next` | `lvgl.indev.get_next([indev])` | Get next input device |

**Input device instance methods:**

| Method | Returns | Description |
|--------|---------|-------------|
| `get_type()` | int | Device type |
| `reset([obj])` | — | Reset device state |
| `reset_long_press()` | — | Reset long press tracking |
| `set_cursor(obj)` | — | Set cursor object |
| `set_group(group)` | — | Assign to focus group |
| `get_point()` | x, y | Current pointer position |
| `get_gesture_dir()` | DIR | Gesture direction |
| `get_key()` | int | Last key pressed |
| `get_scroll_dir()` | DIR | Scroll direction |
| `get_scroll_obj()` | Object | Object being scrolled |
| `get_vect()` | x, y | Pointer movement vector |
| `wait_release()` | — | Wait for input release |

```lua
local indev = lvgl.indev.get_act()
local x, y = indev:get_point()
local key = indev:get_key()
```

---

### Group (`lvgl.group`)

Focus management for keyboard/encoder navigation.

| Function | Signature | Description |
|----------|-----------|-------------|
| `create` | `lvgl.group.create()` | Create new group |
| `get_default` | `lvgl.group.get_default()` | Get default group |
| `remove_obj` | `lvgl.group.remove_obj(obj)` | Remove object from its group |
| `focus_obj` | `lvgl.group.focus_obj(obj)` | Focus specific object |

**Group instance methods:**

| Method | Signature | Description |
|--------|-----------|-------------|
| `delete` | `grp:delete()` | Delete group |
| `set_default` | `grp:set_default()` | Make this the default group |
| `add_obj` | `grp:add_obj(obj)` | Add object to group |
| `remove_obj` | `grp:remove_obj(obj)` | Remove object from group |
| `focus_next` | `grp:focus_next()` | Move focus to next object |
| `focus_prev` | `grp:focus_prev()` | Move focus to previous object |
| `focus_freeze` | `grp:focus_freeze(bool)` | Lock focus on current object |
| `send_data` | `grp:send_data(key_code)` | Simulate key press |
| `set_editing` | `grp:set_editing(bool)` | Enter/exit edit mode |
| `set_wrap` | `grp:set_wrap(bool)` | Enable wrap-around navigation |
| `get_wrap` | `grp:get_wrap()` | Check wrap setting |
| `get_obj_count` | `grp:get_obj_count()` | Number of objects in group |
| `get_focused` | `grp:get_focused()` | Get currently focused object |

```lua
local grp = lvgl.group.get_default()
grp:add_obj(btn1)
grp:add_obj(btn2)
grp:add_obj(btn3)
grp:focus_next()
```

---

### Palette (`lvgl.palette`)

Material Design color palette.

| Function | Signature | Description |
|----------|-----------|-------------|
| `main` | `lvgl.palette.main(color)` | Get main shade |
| `lighten` | `lvgl.palette.lighten(color, level)` | Get lighter shade (level 1-5) |
| `darken` | `lvgl.palette.darken(color, level)` | Get darker shade (level 1-4) |

**Color Names:**

`RED`, `PINK`, `PURPLE`, `DEEP_PURPLE`, `INDIGO`, `BLUE`, `LIGHT_BLUE`, `CYAN`, `TEAL`, `GREEN`, `LIGHT_GREEN`, `LIME`, `YELLOW`, `AMBER`, `ORANGE`, `DEEP_ORANGE`, `BROWN`, `BLUE_GREY`, `GREY`, `NONE`, `LAST`

```lua
obj:set {
    bg_color = lvgl.palette.main(lvgl.palette.BLUE),
    border_color = lvgl.palette.darken(lvgl.palette.BLUE, 2),
}
```

---

### Filesystem (`lvgl.fs`)

File I/O through LVGL's filesystem abstraction.

**Opening files:**

```lua
local f = lvgl.fs.open_file("P/data/config.txt", "r")   -- "r", "w", or "rw"
```

**File methods:**

| Method | Signature | Description |
|--------|-----------|-------------|
| `read` | `f:read("a")` or `f:read(n)` | Read all or n bytes |
| `write` | `f:write(str, ...)` | Write string(s) |
| `seek` | `f:seek(whence, [offset])` | Seek: whence = `"set"`, `"cur"`, `"end"` |
| `tell` | `f:tell()` | Current position |
| `close` | `f:close()` | Close file |

**Opening directories:**

```lua
local dir = lvgl.fs.open_dir("P/data/")
```

**Directory methods:**

| Method | Signature | Description |
|--------|-----------|-------------|
| `read` | `dir:read()` | Returns next entry name or nil |
| `close` | `dir:close()` | Close directory |

```lua
-- Read entire file
local f = lvgl.fs.open_file("P/save.txt", "r")
local content = f:read("a")
f:close()

-- Write file
local f = lvgl.fs.open_file("P/save.txt", "w")
f:write("score=42\n")
f:close()

-- List directory
local dir = lvgl.fs.open_dir("P/apps/")
while true do
    local entry = dir:read()
    if not entry then break end
    print(entry)
end
dir:close()
```

---

## Constants & Enums Reference

### `lvgl.EVENT.*`

| Input Events | Widget Events | Draw Events | Screen Events |
|-------------|---------------|-------------|---------------|
| `PRESSED` | `VALUE_CHANGED` | `DRAW_MAIN_BEGIN` | `SCREEN_LOAD_START` |
| `PRESSING` | `INSERT` | `DRAW_MAIN` | `SCREEN_LOADED` |
| `PRESS_LOST` | `REFRESH` | `DRAW_MAIN_END` | `SCREEN_UNLOAD_START` |
| `SHORT_CLICKED` | `READY` | `DRAW_POST_BEGIN` | `SCREEN_UNLOADED` |
| `LONG_PRESSED` | `CANCEL` | `DRAW_POST` | |
| `LONG_PRESSED_REPEAT` | `DELETE` | `DRAW_POST_END` | |
| `CLICKED` | `CHILD_CHANGED` | | |
| `RELEASED` | `CHILD_CREATED` | | |
| `SCROLL_BEGIN` | `CHILD_DELETED` | | |
| `SCROLL_END` | `SIZE_CHANGED` | | |
| `SCROLL` | `STYLE_CHANGED` | | |
| `GESTURE` | `LAYOUT_CHANGED` | | |
| `KEY` | `GET_SELF_SIZE` | | |
| `FOCUSED` | `HIT_TEST` | | |
| `DEFOCUSED` | `COVER_CHECK` | | |
| `LEAVE` | `REFR_EXT_DRAW_SIZE` | | |

Special: `ALL` — catch every event.

### `lvgl.FLAG.*`

| Interaction | Scrolling | Layout | Custom |
|-------------|-----------|--------|--------|
| `HIDDEN` | `SCROLLABLE` | `IGNORE_LAYOUT` | `USER_1` |
| `CLICKABLE` | `SCROLL_ELASTIC` | `FLOATING` | `USER_2` |
| `CLICK_FOCUSABLE` | `SCROLL_MOMENTUM` | `OVERFLOW_VISIBLE` | `USER_3` |
| `CHECKABLE` | `SCROLL_ONE` | `LAYOUT_1` | `USER_4` |
| `PRESS_LOCK` | `SCROLL_CHAIN_HOR` | `LAYOUT_2` | `WIDGET_1` |
| `SNAPPABLE` | `SCROLL_CHAIN_VER` | | `WIDGET_2` |
| `EVENT_BUBBLE` | `SCROLL_CHAIN` | | |
| `GESTURE_BUBBLE` | `SCROLL_ON_FOCUS` | | |
| `ADV_HITTEST` | `SCROLL_WITH_ARROW` | | |

### `lvgl.STATE.*`

`DEFAULT`, `CHECKED`, `FOCUSED`, `FOCUS_KEY`, `EDITED`, `HOVERED`, `PRESSED`, `SCROLLED`, `DISABLED`, `USER_1`, `USER_2`, `USER_3`, `USER_4`, `ANY`

### `lvgl.PART.*`

`MAIN`, `SCROLLBAR`, `INDICATOR`, `KNOB`, `SELECTED`, `ITEMS`, `CURSOR`, `CUSTOM_FIRST`, `ANY`

### `lvgl.ALIGN.*`

**Inner alignment:**
`DEFAULT`, `TOP_LEFT`, `TOP_MID`, `TOP_RIGHT`, `BOTTOM_LEFT`, `BOTTOM_MID`, `BOTTOM_RIGHT`, `LEFT_MID`, `RIGHT_MID`, `CENTER`

**Outer alignment (for `align_to`):**
`OUT_TOP_LEFT`, `OUT_TOP_MID`, `OUT_TOP_RIGHT`, `OUT_BOTTOM_LEFT`, `OUT_BOTTOM_MID`, `OUT_BOTTOM_RIGHT`, `OUT_LEFT_TOP`, `OUT_LEFT_MID`, `OUT_LEFT_BOTTOM`, `OUT_RIGHT_TOP`, `OUT_RIGHT_MID`, `OUT_RIGHT_BOTTOM`

### `lvgl.KEY.*`

`UP`, `DOWN`, `RIGHT`, `LEFT`, `ESC`, `DEL`, `BACKSPACE`, `ENTER`, `NEXT`, `PREV`, `HOME`, `END`

### `lvgl.DIR.*`

`NONE`, `LEFT`, `RIGHT`, `TOP`, `BOTTOM`, `HOR`, `VER`, `ALL`

### `lvgl.FLEX_FLOW.*`

`ROW`, `COLUMN`, `ROW_WRAP`, `ROW_REVERSE`, `ROW_WRAP_REVERSE`, `COLUMN_WRAP`, `COLUMN_REVERSE`, `COLUMN_WRAP_REVERSE`

### `lvgl.FLEX_ALIGN.*`

`START`, `END`, `CENTER`, `SPACE_EVENLY`, `SPACE_AROUND`, `SPACE_BETWEEN`

### `lvgl.GRID_ALIGN.*`

`START`, `CENTER`, `END`, `STRETCH`, `SPACE_EVENLY`, `SPACE_AROUND`, `SPACE_BETWEEN`

### `lvgl.SCROLLBAR_MODE.*`

`OFF`, `ON`, `ACTIVE`, `AUTO`

### `lvgl.KEYBOARD_MODE.*`

`TEXT_LOWER`, `TEXT_UPPER`, `SPECIAL`, `NUMBER`, `USER_1`, `USER_2`, `USER_3`, `USER_4`, `TEXT_ARABIC` (if enabled)

### `lvgl.ROLLER_MODE.*`

`NORMAL`, `INFINITE`

### `lvgl.SCR_LOAD_ANIM.*`

`NONE`, `OVER_LEFT`, `OVER_RIGHT`, `OVER_TOP`, `OVER_BOTTOM`, `MOVE_LEFT`, `MOVE_RIGHT`, `MOVE_TOP`, `MOVE_BOTTOM`, `FADE_ON`, `FADE_IN`, `FADE_OUT`, `OUT_LEFT`, `OUT_RIGHT`, `OUT_TOP`, `OUT_BOTTOM`

### `lvgl.LABEL_CONST.*`

`LONG_WRAP`, `LONG_DOT`, `LONG_SCROLL`, `LONG_SCROLL_CIRCULAR`, `LONG_CLIP`

### `lvgl.COLOR_FORMAT.*` (for Canvas)

`RGB565`, `RGB888`, `ARGB8888`, `XRGB8888`, `L8`, `A8`, `I1`, `I2`, `I4`, `I8`

### `lvgl.BUILTIN_FONT.*`

`DEFAULT`, `MONTSERRAT_8` through `MONTSERRAT_48` (even sizes), `MONTSERRAT_28_COMPRESSED`, `DEJAVU_16_PERSIAN_HEBREW`, `SIMSUN_16_CJK`, `UNSCII_8`, `UNSCII_16`

### Standalone Constants

| Constant | Description |
|----------|-------------|
| `lvgl.ANIM_REPEAT_INFINITE` | Infinite animation repeat |
| `lvgl.ANIM_PLAYTIME_INFINITE` | Infinite animation playtime |
| `lvgl.SIZE_CONTENT` | Size to fit content |
| `lvgl.RADIUS_CIRCLE` | Fully rounded corners |
| `lvgl.COORD_MAX` | Maximum coordinate value |
| `lvgl.COORD_MIN` | Minimum coordinate value |
| `lvgl.LV_ZOOM_NONE` | No zoom (100%) = 256 |
| `lvgl.BTNMATRIX_BTN_NONE` | No button selected |
| `lvgl.CHART_POINT_NONE` | No chart point |
| `lvgl.DROPDOWN_POS_LAST` | Insert at end of dropdown |
| `lvgl.LABEL_DOT_NUM` | Number of dots for LONG_DOT |
| `lvgl.LABEL_POS_LAST` | Position at end of label |
| `lvgl.LABEL_TEXT_SELECTION_OFF` | Disable text selection |
| `lvgl.TABLE_CELL_NONE` | No table cell |
| `lvgl.TEXTAREA_CURSOR_LAST` | Cursor at end of textarea |
| `lvgl.LAYOUT_FLEX` | Flex layout type |
| `lvgl.LAYOUT_GRID` | Grid layout type |

### Utility Functions

| Function | Signature | Description |
|----------|-----------|-------------|
| `lvgl.OPA(n)` | `lvgl.OPA(100)` → 255 | Convert 0-100 opacity to 0-255 |
| `lvgl.PCT(n)` | `lvgl.PCT(50)` | Percentage of parent size |
| `lvgl.HOR_RES()` | Returns 320 | Screen width |
| `lvgl.VER_RES()` | Returns 240 | Screen height |
