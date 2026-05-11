# Keyboard Input for Lua Apps

The T-Deck keyboard can be read three ways in Lua. Choose the method that fits your app.

## Method 1: LVGL Key Events (Multiple Keys, Event-Driven)

LVGL fires a `KEY` event whenever a key is pressed. Your app registers a callback on a focused widget and reacts to each keypress as it arrives. This is the standard approach used by most apps and supports the full keyboard.

### Setup

```lua
-- Make the screen focusable and register it with the default group
scr:add_flag(lvgl.FLAG.CLICKABLE)
scr:add_flag(lvgl.FLAG.CLICK_FOCUSABLE)
local group = lvgl.group.get_default()
group:add_obj(scr)
lvgl.group.focus_obj(scr)

-- Listen for key events
scr:onevent(lvgl.EVENT.KEY, function(obj, code)
    local indev = lvgl.indev.get_act()
    local key = indev:get_key()

    if key == lvgl.KEY.ENTER then
        -- enter was pressed
    elseif key == 119 or key == 87 then
        -- 'w' or 'W' was pressed
    end
end)
```

### How It Works

- LVGL delivers one event per keypress through the focused widget
- You get the key code via `lvgl.indev.get_act():get_key()`
- Special keys use LVGL constants: `lvgl.KEY.ENTER`, `lvgl.KEY.UP`, `lvgl.KEY.DOWN`, `lvgl.KEY.LEFT`, `lvgl.KEY.RIGHT`, `lvgl.KEY.BACKSPACE`, `lvgl.KEY.ESC`
- Regular keys use ASCII codes (e.g., `string.byte("a")` = 97)
- LVGL handles ENTER long press internally via `lvgl.EVENT.LONG_PRESSED`

### When to Use

- UI navigation and text input
- Menu selection
- Any app where you react to individual keypresses one at a time
- When you only need one key at a time

### Example: Snake Game Input

The Snake game uses this method. Direction changes are queued on each keypress and applied on the next game tick:

```lua
scr:onevent(lvgl.EVENT.KEY, function(obj, code)
    local indev = lvgl.indev.get_act()
    local key = indev:get_key()

    if key == lvgl.KEY.ENTER then
        startGame()
        return
    end

    -- WASD or arrow keys to change direction
    -- w=119 W=87, a=97 A=65, s=115 S=83, d=100 D=68
    local dx, dy = snake.dir.x, snake.dir.y
    if (key == lvgl.KEY.UP or key == 119 or key == 87) and dy ~= 1 then
        snake.next_dir = {x = 0, y = -1}
    elseif (key == lvgl.KEY.DOWN or key == 115 or key == 83) and dy ~= -1 then
        snake.next_dir = {x = 0, y = 1}
    elseif (key == lvgl.KEY.LEFT or key == 97 or key == 65) and dx ~= 1 then
        snake.next_dir = {x = -1, y = 0}
    elseif (key == lvgl.KEY.RIGHT or key == 100 or key == 68) and dx ~= -1 then
        snake.next_dir = {x = 1, y = 0}
    end
end)
```

---

## Method 2: LVGL Press/Release Events (Single Button + Touchscreen)

LVGL fires `PRESSED` and `RELEASED` events on clickable widgets. Because these events are triggered by both the touchscreen and the ENTER key, this approach gives you unified input from both sources through a single handler. Ideal for one-button games and apps where tap/click is the only interaction.

### Setup

```lua
-- Make a widget clickable, add to group, and focus it
local bgLayer = lvgl.Object(parent, { w = 320, h = 240 })
bgLayer:add_flag(lvgl.FLAG.CLICKABLE)
bgLayer:add_flag(lvgl.FLAG.CLICK_FOCUSABLE)
local group = lvgl.group.get_default()
group:add_obj(bgLayer)
lvgl.group.focus_obj(bgLayer)

-- Handle press and release from both touch and ENTER key
bgLayer:onevent(lvgl.EVENT.PRESSED, function(obj, code)
    on_press()
end)
bgLayer:onevent(lvgl.EVENT.RELEASED, function(obj, code)
    on_release()
end)
```

### How It Works

- LVGL treats ENTER keypresses and touchscreen taps the same way on clickable widgets
- `PRESSED` fires when the user touches the screen OR presses ENTER
- `RELEASED` fires when they lift their finger OR release ENTER
- No key code checking needed — there is only one action (press/release)
- LVGL handles all the input routing internally

### When to Use

- One-button games (tap to flap, tap to jump)
- Any app where touch and ENTER should do the same thing
- When you need press AND release timing (hold-to-charge with a single button)
- Simple interactions that don't need specific key identification

### Example: Flappy Bird Input

The Flappy Bird game uses this method. Pressing applies upward force to the bird, releasing lets it fall. Both touch and ENTER work identically:

```lua
-- Bird physics tied to press/release
bird.pressed = function(self) bird:applyForce(-9); bird.velocity = 0 end
bird.released = function(self) bird:applyForce(5); bird.velocity = 0 end

-- Background layer receives both touch and ENTER events
bgLayer:onevent(lvgl.EVENT.PRESSED, function(obj, code)
    if not game.running or not game.playing then return end
    bird:pressed()
end)
bgLayer:onevent(lvgl.EVENT.RELEASED, function(obj, code)
    if not game.running or not game.playing then return end
    bird:released()
end)
```

---

## Method 3: Direct Key State Functions (Multi-Key, Poll-Driven)

These functions read the physical state of any key directly from the keyboard matrix. They support multiple simultaneous keys and detect press, release, and hold transitions. Use them in a polling timer.

### Functions

| Function | Returns | Description |
|----------|---------|-------------|
| `_kb_is_down(key)` | bool | Is the key physically pressed right now? |
| `_kb_just_pressed(key)` | bool | Was the key pressed this poll cycle? (one-shot) |
| `_kb_just_released(key)` | bool | Was the key released this poll cycle? (one-shot) |
| `_kb_is_held(key)` | bool | Has the key been held for more than 400ms? |
| `_kb_hold_duration(key)` | int | Milliseconds since the key was first pressed (0 if not pressed) |

All functions take a key code as their argument:
- Letters: `string.byte("a")` through `string.byte("z")` (lowercase)
- Uppercase: `string.byte("A")` through `string.byte("Z")` (when shift is held)
- Symbols: `string.byte("$")`, `string.byte("#")`, etc. (sym layer)
- Enter: `0x0D` (13)
- Backspace: `0x08` (8)
- Space: `string.byte(" ")` (32)

### Modifier Keys

These functions check the physical state of modifier keys. They take no arguments.

| Function | Returns | Description |
|----------|---------|-------------|
| `_kb_shift()` | bool | Is either shift key pressed? |
| `_kb_lshift()` | bool | Is the left shift key pressed? |
| `_kb_rshift()` | bool | Is the right shift key pressed? |
| `_kb_sym()` | bool | Is the sym key pressed? |
| `_kb_alt()` | bool | Is the alt key pressed? |

Modifier keys also affect character resolution: shift uppercases letters, sym switches to the symbol layer. These functions let you additionally read the modifier state directly for use as game buttons or custom combos.

### Basic Usage

```lua
-- Poll keyboard state in a timer
local input_timer = lvgl.Timer { period = 33, cb = function()
    -- Continuous movement while key is held
    if _kb_is_down(string.byte("a")) then move_left() end
    if _kb_is_down(string.byte("d")) then move_right() end
    if _kb_is_down(string.byte("w")) then move_up() end
    if _kb_is_down(string.byte("s")) then move_down() end

    -- One-shot action on press (fires once per keypress)
    if _kb_just_pressed(string.byte(" ")) then jump() end

    -- Action on release
    if _kb_just_released(string.byte("x")) then release_charge() end

    -- Long press detection
    if _kb_is_held(string.byte("m")) then open_menu() end

    -- Modifier keys as game buttons
    local speed = _kb_shift() and 2 or 1
    if _kb_is_down(string.byte("w")) then move_up(speed) end

    -- Left/right shift as separate buttons
    if _kb_lshift() then strafe_left() end
    if _kb_rshift() then strafe_right() end

    -- Alt as secondary action modifier
    if _kb_just_pressed(string.byte(" ")) then
        if _kb_alt() then special_attack() else jump() end
    end
end }
```

### When to Use

- Games that need real-time continuous input (hold a key to keep moving)
- Apps that need multiple simultaneous keys (e.g., diagonal movement with W+D)
- Hold-to-charge or hold-to-activate mechanics
- Any input that LVGL's single-key event model can't handle

### Example: Game with Multi-Key Input

```lua
local FIRE = string.byte(" ")
local LEFT = string.byte("a")
local RIGHT = string.byte("d")
local UP = string.byte("w")
local DOWN = string.byte("s")

local game_timer = lvgl.Timer { period = 33, cb = function()
    -- Diagonal movement: W+D = up-right (both keys detected simultaneously)
    local dx, dy = 0, 0
    if _kb_is_down(LEFT) then dx = dx - 1 end
    if _kb_is_down(RIGHT) then dx = dx + 1 end
    if _kb_is_down(UP) then dy = dy - 1 end
    if _kb_is_down(DOWN) then dy = dy + 1 end
    if dx ~= 0 or dy ~= 0 then
        player:move(dx, dy)
    end

    -- Fire once per press, not continuously
    if _kb_just_pressed(FIRE) then
        player:shoot()
    end

    -- Charge attack: hold to charge, release to fire
    if _kb_is_down(string.byte("x")) then
        local charge = _kb_hold_duration(string.byte("x"))
        player:charge(charge)
    end
    if _kb_just_released(string.byte("x")) then
        player:release_charged_attack()
    end
end }
```

---

## Comparison

| | Method 1: Key Events | Method 2: Press/Release | Method 3: Direct Key State |
|---|---|---|---|
| Input model | Event callback | Event callback | Poll-driven timer |
| Keys supported | All keys | ENTER + touchscreen | All keys |
| Multiple keys | One at a time | One button only | Yes (simultaneous) |
| Touch support | No | Yes (unified) | No |
| Hold detection | Only ENTER | Via PRESSED duration | Any key |
| Press/release edges | No | Yes (PRESSED/RELEASED) | Yes (`just_pressed`/`just_released`) |
| Best for | UI, menus, text input | One-button games, tap interactions | Multi-key games, real-time controls |

Methods can be combined in the same app. For example, use Method 1 for menu navigation, Method 2 for a simple tap-to-start screen, and Method 3 during gameplay that needs multi-key input.
