--  Navigation scope stack + touch-input helpers
-- Thin Lua front for the C nav controller (a STACK of gridnav scopes). The top
-- scope is the interactive one; pushing suspends the scope below, popping
-- resumes it. Apps should prefer these over the raw _nav_* bindings:
--
--   nav.replace(cont, opts)  -- swap the active view (top scope)   [≈ _nav_setup]
--   nav.push(cont, opts)     -- open a popup / sub-scope over the current one
--   nav.pop()                -- close the top scope, resume the one beneath
--   nav.reset()              -- tear down the whole stack (app exit)
--   nav.list(list, opts)     -- wire a scroll list's tap→row-select, 'q'→back
--   nav.tap(obj, fn)         -- tap-not-swipe activation for any widget
--   nav.scroll_aware(list, on_settle) -> binder(row, fn) -- scroll-aware row taps
--
-- opts = { flags = nav.ROLLOVER (default), focus = <child>, preserve = <bool> }.
-- `preserve` keeps a scrolled list's position and focuses the first on-screen
-- row instead of snapping to the top. All calls are safe from inside an event
-- handler — the controller defers the unsafe gridnav bits to a safe point.
--
-- Why a stack: a single global container could not represent nesting (a popup
-- over a view, a row-select list over its controls), so apps hand-rolled a
-- save/restore dance and a single deferred-removal slot that could clobber
-- itself. The stack owns both. See nav_controller_pitfalls / main.cpp.

local lvgl = require("lvgl")

local nav = {}

-- Gridnav control flags (globals published by the C side at boot).
nav.ROLLOVER     = GRIDNAV_ROLLOVER
nav.SCROLL_FIRST = GRIDNAV_SCROLL_FIRST
nav.NONE         = GRIDNAV_NONE

-- Swap the active view in place (no new stack level). Use for view-to-view
-- navigation within an app. The outgoing container should be torn down by the
-- caller (e.g. apps.delete_view) — nav does not delete it.
function nav.replace(cont, opts)
    opts = opts or {}
    _nav_setup(cont, opts.flags or nav.ROLLOVER, opts.preserve)
end

-- Open a nested scope over the current one. The parent is suspended (dropped
-- from the focus group; its gridnav deferred-removed) and restored on pop.
function nav.push(cont, opts)
    opts = opts or {}
    _nav_push(cont, opts.flags or nav.ROLLOVER, opts.focus, opts.preserve)
end

-- Close the top scope and resume the one beneath it. Delete the closing
-- container yourself afterwards (e.g. overlay:delete()).
function nav.pop()
    _nav_pop()
end

-- Tear down every scope (app exit / full teardown).
function nav.reset()
    _nav_reset()
end

function nav.set_focused(child)
    _nav_set_focused(child)
end

function nav.is_active()
    return _nav_is_active()
end

-- Wire a scroll list for the touch + trackball/keyboard idiom: a tap (or click)
-- enters row-select (pushes a scope onto the list so arrows/trackball step
-- through rows), and 'q' returns focus to the controls beneath. Row activation
-- itself is bound by the caller per row (e.g. via a scroll-aware tap binder);
-- this only manages the scope transitions. Returns a function that force-exits
-- row-select (call it before rebuilding the list's rows).
function nav.list(list, opts)
    opts = opts or {}
    local entered = false
    -- Leave row-select and resume the controls beneath. No-op when not in select
    -- mode. Returned so a row's own action (e.g. "item chosen") can exit too, not
    -- just the 'q' key.
    local function exit_select()
        if not entered then return end
        entered = false
        nav.pop()
    end
    list:add_flag(lvgl.FLAG.CLICK_FOCUSABLE)
    list:onevent(lvgl.EVENT.RELEASED, function()
        if entered then return end
        entered = true
        nav.push(list, { flags = opts.flags or nav.ROLLOVER, preserve = true })
    end)
    list:onevent(lvgl.EVENT.KEY, function()
        if lvgl.indev.get_act():get_key() == 113 then exit_select() end
    end)
    return exit_select
end

-- ── Touchscreen tap-vs-swipe (any widget, not just nav scopes) ──────────────
-- Fire `activate` on a tap (a near-stationary touch press/release) but NOT when
-- the finger drags to scroll a parent list/page. The decision is finger travel
-- between PRESSED and RELEASED — not LVGL's scroll flag — so it can't get stuck
-- (a flag set on SCROLL_BEGIN that fails to clear) and a small wobble still
-- counts as a tap. The trackball/keyboard (a non-pointer indev) always fires.

-- lv_indev_type_t value for a touchscreen/mouse pointer (vs keypad/encoder).
local INDEV_POINTER = 1
-- Max finger travel (px, squared) from press to release still counted as a tap.
nav.TAP_SLOP_SQ = 16 * 16

-- Bind a tap-vs-swipe activation to one widget (button, row, bubble, ...).
function nav.tap(obj, activate)
    local px, py  -- press point, pointer (touch) only
    obj:onevent(lvgl.EVENT.PRESSED, function()
        local indev = lvgl.indev.get_act()
        if indev and indev:get_type() == INDEV_POINTER then
            px, py = indev:get_point()
        else
            px, py = nil, nil
        end
    end)
    obj:onevent(lvgl.EVENT.RELEASED, function()
        local indev = lvgl.indev.get_act()
        -- Trackball / keyboard (non-pointer): always activate.
        if not (indev and indev:get_type() == INDEV_POINTER) then
            activate()
            return
        end
        -- Touch: only a near-stationary press (a tap) activates; a drag scrolls.
        if px == nil then activate(); return end
        local x, y = indev:get_point()
        local dx, dy = x - px, y - py
        if dx * dx + dy * dy <= nav.TAP_SLOP_SQ then
            activate()
        end
    end)
end

-- For a scrollable list: returns a binder( row, activate ) that attaches a
-- scroll-aware tap (nav.tap) to each row, so a tap opens a row but a drag
-- scrolls past it. `on_settle` (optional) runs from the list's single SCROLL_END
-- handler — route windowed-paging work through it, since luavgl allows only ONE
-- callback per event code per object (a second SCROLL_END would replace this).
function nav.scroll_aware(list, on_settle)
    if on_settle then
        list:onevent(lvgl.EVENT.SCROLL_END, function() on_settle() end)
    end
    return function(obj, activate)
        nav.tap(obj, activate)
    end
end

return nav
