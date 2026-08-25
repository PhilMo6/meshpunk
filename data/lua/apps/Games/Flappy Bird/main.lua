local lvgl = require("lvgl")
local sound = require("lib/sound")
local apps = require("lib/apps")

-- Receive app directory from launcher (e.g. "L:/lua/apps/Games/Flappy Bird")
local app_dir = ...

-- ============================================================
-- Flappy Bird
--
-- Single fixed-step timer drives everything; there are no lvgl
-- Anims, so the manager's timer teardown covers every exit path
-- (quit button, home chord, app-to-app launch).
--
-- Every PNG is decoded exactly once at startup into app-owned
-- canvases (the atlas + the two tiled scroll strips). All
-- runtime rendering references those buffers via get_image(),
-- so nothing touches the filesystem or LVGL's image cache after
-- startup. bg_day.png / land.png are 64px-wide repeating tiles
-- and must be tiled across the strip canvases to cover the
-- screen.
-- ============================================================

local W = lvgl.HOR_RES()
local H = lvgl.VER_RES()

local IMAGE_PATH = app_dir .. "/"

local floor = math.floor
local random = math.random
local sin = math.sin

-- Fixed timestep; all speeds are px/second scaled by DT each tick.
local TICK_MS = 33
local DT = TICK_MS / 1000

local LAND_H   = 56
local GROUND_Y = H - LAND_H          -- top edge of the ground strip

-- Bird
local BIRD_W, BIRD_H = 18, 13
local BIRD_X    = floor(W / 3) - floor(BIRD_W / 2)
local HIT_INSET = 2                  -- hitbox shrink per side
local GRAVITY   = 900                -- px/s^2
local FLAP_VY   = -260               -- impulse velocity per tap
local VY_MAX    = 470                -- terminal fall speed
local CEILING   = 2                  -- top clamp (does not kill)

-- Pipes
local PIPE_W, PIPE_H = 26, 160
local PIPE_N     = 4
local STRIDE     = floor(W / 3) + PIPE_W   -- distance between pipe pairs
local GAP_MAX    = 70                -- gap at score 0
local GAP_MIN    = 52
local GAP_MARGIN = 28                -- min pipe visible above/below the gap

-- World speed
local SPEED0     = 105               -- px/s at score 0
local SPEED_MAX  = 150
local SPEED_RAMP = 1.2               -- px/s gained per point
local SKY_PARALLAX = 0.25            -- sky scroll fraction of ground speed

local S_MENU, S_READY, S_PLAY, S_DYING, S_OVER = 1, 2, 3, 4, 5

local game = {
    running  = true,
    state    = S_MENU,
    muted    = false,
    sounds   = {},
    flash_t  = 0,
    over_y   = 0,
    new_best = false,
}

local scene = {}          -- widget handles
local atlas = {}          -- sprite name -> { src = lightuserdata, w, h }
local atlas_store         -- hidden container owning the atlas canvases
local pipes               -- array of { x, gy, gap, scored, xi, top, bot }
local best, score = 0, 0
local WING_SEQ            -- bird frame srcs in flap order
local SKY_TILE_W, LAND_TILE_W = 64, 64

local bird = {
    y = 0, vy = 0, rot = 0,
    frame = 1, ft = 0,        -- wing frame index + tick divider
    bob = 0, base = 0,        -- menu/ready idle bob
    last_y = -1,
}

local world = {
    speed = SPEED0,
    land_off = 0, sky_off = 0,
    land_x = 1, sky_x = 1,    -- last applied int positions
    phase = 0,                -- 0 = day sky, 1 = night sky
}

local enter_menu, enter_ready, enter_dying, enter_over

-- ── Persistence (formats shared with v1.x: bare number / "1"/"0") ───────────
local function load_score()
    local f = io.open(app_dir .. "/save.txt", "r")
    if not f then return 0 end
    local txt = f:read("*a")
    f:close()
    return tonumber(txt) or 0
end

local function save_score(s)
    local f = io.open(app_dir .. "/save.txt", "w")
    if f then f:write(tostring(s)) f:close() end
end

local function load_muted()
    local f = io.open(app_dir .. "/mute.txt", "r")
    if not f then return false end
    local txt = f:read("*a")
    f:close()
    return txt == "1"
end

local function save_muted(m)
    local f = io.open(app_dir .. "/mute.txt", "w")
    if f then f:write(m and "1" or "0") f:close() end
end

-- ── Reused property tables ──────────────────────────────────────────────────
-- obj:set() reads keys at call time, so one table per shape avoids allocating
-- a table on every tick.
local T_X   = { x = 0 }
local T_Y   = { y = 0 }
local T_XY  = { x = 0, y = 0 }
local T_SRC = {}
local T_ROT = { rotation = 0 }
local T_TXT = {}

local function set_txt(lbl, s)
    T_TXT.text = s
    lbl:set(T_TXT)
end

-- ── Sounds ──────────────────────────────────────────────────────────────────
local function sfx(s)
    if s and not game.muted then s:play() end
end

local function make_sounds()
    local list = {}
    local function reg(s)
        if s then list[#list + 1] = s end
        return s
    end

    -- Short blip per tap (no looping tone: taps are instantaneous)
    game.snd_flap = reg(sound.generateTone(520, 70, {
        end_freq = 310, waveform = "triangle",
        attack = 2, decay = 40, sustain = 0.0, release = 20,
    }))

    game.snd_point = reg(sound.generateMelody({
        { freq = 988, ms = 45 }, { freq = 0, ms = 15 }, { freq = 1319, ms = 70 },
    }, { waveform = "square", attack = 2, decay = 30, sustain = 0.5, release = 20 }))

    game.snd_hit = reg(sound.generateTone(200, 90, {
        end_freq = 70, waveform = "square",
        attack = 1, decay = 60, sustain = 0.3, release = 30,
    }))

    -- Game over: chromatic descent, plays once
    game.snd_die = reg(sound.generateMelody({
        {freq=659, ms=250}, {freq=0, ms=30},
        {freq=622, ms=250}, {freq=0, ms=30},
        {freq=587, ms=250}, {freq=0, ms=30},
        {freq=523, ms=400}, {freq=0, ms=50},
        {freq=494, ms=250}, {freq=0, ms=30},
        {freq=440, ms=250}, {freq=0, ms=30},
        {freq=392, ms=400}, {freq=0, ms=50},
        {freq=330, ms=700},
    }, { waveform = "square", attack = 10, decay = 80, sustain = 0.4, release = 100 }))

    game.sounds = list
end

-- ── Sprite atlas ────────────────────────────────────────────────────────────
local SPRITES = {
    "bird1", "bird2", "bird3", "pipe_up", "pipe_down",
    "medals", "score", "text_game_over", "title",
    "button_play", "button_quit",
}

local function load_sprites(root)
    atlas_store = root:Object{ w = 1, h = 1, bg_opa = 0, border_width = 0, pad_all = 0 }
    atlas_store:clear_flag(lvgl.FLAG.SCROLLABLE)
    atlas_store:add_flag(lvgl.FLAG.HIDDEN)

    local probe = atlas_store:Image{}
    for _, name in ipairs(SPRITES) do
        local path = IMAGE_PATH .. name .. ".png"
        local w, h = probe:get_img_size(path)
        if not w or not h then
            error("asset failed to load: " .. name .. ".png", 0)
        end
        local c = atlas_store:Canvas{ w = w, h = h, cf = lvgl.COLOR_FORMAT.ARGB8888 }
        c:fill_bg("#000000", 0)
        c:draw_image{ x1 = 0, y1 = 0, x2 = w - 1, y2 = h - 1, src = path, opa = 255 }
        atlas[name] = { src = c:get_image(), w = w, h = h }
    end
    probe:delete()
end

local function decorate_night(c, w, h)
    c:draw_rect{ x1 = 0, y1 = 0, x2 = w - 1, y2 = h - 1,
                 bg_color = "#0a1233", bg_opa = 165 }
    for _ = 1, 42 do
        local sx = random(0, w - 2)
        local sy = random(3, floor(h * 0.55))
        local s = random(1, 2)
        c:draw_rect{ x1 = sx, y1 = sy, x2 = sx + s - 1, y2 = sy + s - 1,
                     bg_color = "#FFFFFF", bg_opa = random(120, 255) }
    end
end

-- Tile a strip image across a canvas one tile wider than the screen; scrolling
-- wraps x over one tile width so the seam never shows. Opaque tiles -> RGB565.
local function make_scroll_strip(root, path, y, night)
    local probe = atlas_store:Image{}
    local tw, th = probe:get_img_size(path)
    probe:delete()
    if not tw or not th then
        error("asset failed to load: " .. path, 0)
    end
    local w = W + tw
    local c = root:Canvas{ w = w, h = th, cf = lvgl.COLOR_FORMAT.RGB565, x = 0, y = y }
    for x = 0, w - 1, tw do
        c:draw_image{ x1 = x, y1 = 0, x2 = x + tw - 1, y2 = th - 1, src = path, opa = 255 }
    end
    if night then decorate_night(c, w, th) end
    return c, tw
end

-- ── Small helpers ───────────────────────────────────────────────────────────
local function gap_for(s)
    local g = GAP_MAX - floor(s / 3) * 2
    if g < GAP_MIN then g = GAP_MIN end
    return g
end

local function rand_gap_y(gap)
    return random(GAP_MARGIN, GROUND_Y - gap - GAP_MARGIN)
end

local function place_pipe(p)
    local xi = floor(p.x)
    p.xi = xi
    T_XY.x = xi
    T_XY.y = p.gy - PIPE_H
    p.top:set(T_XY)
    T_XY.y = p.gy + p.gap
    p.bot:set(T_XY)
end

local function set_phase(phase)
    if phase == world.phase then return end
    world.phase = phase
    if phase == 1 then
        scene.sky_day:add_flag(lvgl.FLAG.HIDDEN)
        scene.sky_night:clear_flag(lvgl.FLAG.HIDDEN)
    else
        scene.sky_night:add_flag(lvgl.FLAG.HIDDEN)
        scene.sky_day:clear_flag(lvgl.FLAG.HIDDEN)
    end
end

local function scroll_world(dx)
    local w = world
    w.land_off = (w.land_off + dx) % LAND_TILE_W
    local lx = -floor(w.land_off)
    if lx ~= w.land_x then
        w.land_x = lx
        T_X.x = lx
        scene.land:set(T_X)
    end
    w.sky_off = (w.sky_off + dx * SKY_PARALLAX) % SKY_TILE_W
    local sx = -floor(w.sky_off)
    if sx ~= w.sky_x then
        w.sky_x = sx
        T_X.x = sx
        scene.sky_day:set(T_X)
        scene.sky_night:set(T_X)
    end
end

local function animate_wings()
    bird.ft = bird.ft + 1
    if bird.ft >= 4 then
        bird.ft = 0
        local f = bird.frame % 4 + 1
        bird.frame = f
        T_SRC.src = WING_SEQ[f]
        scene.bird:set(T_SRC)
    end
end

-- ── Overlays (menu / game over) ─────────────────────────────────────────────
local function quit_app()
    if not game.running then return end
    game.running = false
    apps.go_home()   -- runs the on_close cleanup, then deletes timers + root
end

local function close_overlay()
    if scene.overlay then
        scene.overlay:delete()
        scene.overlay = nil
        lvgl.group.focus_obj(scene.root)
    end
end

local function make_overlay()
    local cont = scene.root:Object{ w = W, h = H, bg_opa = 0, border_width = 0, pad_all = 0 }
    cont:clear_flag(lvgl.FLAG.CLICKABLE)
    cont:clear_flag(lvgl.FLAG.SCROLLABLE)
    scene.overlay = cont
    return cont
end

-- Buttons must be direct children of the gridnav'd container.
local function arm_overlay(cont)
    _gridnav_add(cont, GRIDNAV_ROLLOVER)
    local grp = lvgl.group.get_default()
    grp:add_obj(cont)
    lvgl.group.focus_obj(cont)
end

local function image_button(parent, sprite, cb)
    local btn = parent:Image{ src = sprite.src }
    -- gridnav focus needs CLICKABLE and CLICK_FOCUSABLE together; image
    -- widgets carry neither by default.
    btn:add_flag(lvgl.FLAG.CLICKABLE)
    btn:add_flag(lvgl.FLAG.CLICK_FOCUSABLE)
    btn:onevent(lvgl.EVENT.PRESSED, function()
        if not game.running then return end
        cb()
    end)
    return btn
end

-- ── Input ───────────────────────────────────────────────────────────────────
local function flap()
    bird.vy = FLAP_VY
    bird.rot = -300           -- tenths of a degree
    T_ROT.rotation = -300
    scene.bird:set(T_ROT)
    sfx(game.snd_flap)
end

local function start_play()
    game.state = S_PLAY
    scene.ready1:add_flag(lvgl.FLAG.HIDDEN)
    scene.ready2:add_flag(lvgl.FLAG.HIDDEN)
end

local function on_tap()
    if not game.running then return end
    local st = game.state
    if st == S_PLAY then
        flap()
    elseif st == S_READY then
        start_play()
        flap()
    end
end

-- Any key flaps except the trackball's arrow keys (nudging the trackball must
-- not count as a tap). Trackball click arrives as PRESSED, not KEY.
local function on_key()
    if not game.running then return end
    local indev = lvgl.indev.get_act()
    local key = indev and indev:get_key() or 0
    if key == lvgl.KEY.UP or key == lvgl.KEY.DOWN
       or key == lvgl.KEY.LEFT or key == lvgl.KEY.RIGHT
       or key == lvgl.KEY.ESC then
        return
    end
    on_tap()
end

-- ── Scoring ─────────────────────────────────────────────────────────────────
local function add_score()
    score = score + 1
    set_txt(scene.score, tostring(score))
    sfx(game.snd_point)
    if score % 10 == 0 then
        set_phase(floor(score / 10) % 2)
    end
    world.speed = SPEED0 + score * SPEED_RAMP
    if world.speed > SPEED_MAX then world.speed = SPEED_MAX end
end

-- ── State transitions ───────────────────────────────────────────────────────
enter_menu = function()
    game.state = S_MENU
    local cont = make_overlay()

    local title = cont:Image{ src = atlas.title.src }
    title:set{ align = { type = lvgl.ALIGN.TOP_MID, y_ofs = floor(H * 0.15) } }

    local play = image_button(cont, atlas.button_play, enter_ready)
    play:set{ align = { type = lvgl.ALIGN.CENTER, y_ofs = floor(H / 6) } }

    local quit = image_button(cont, atlas.button_quit, quit_app)
    quit:set{ align = { type = lvgl.ALIGN.TOP_RIGHT } }

    arm_overlay(cont)
end

enter_ready = function()
    close_overlay()
    game.state = S_READY
    score = 0
    game.new_best = false
    world.speed = SPEED0

    bird.vy = 0
    bird.rot = 0
    bird.frame = 1
    bird.ft = 0
    bird.bob = 0
    bird.y = bird.base
    bird.last_y = bird.base
    T_XY.x = BIRD_X
    T_XY.y = bird.base
    scene.bird:set(T_XY)
    T_ROT.rotation = 0
    scene.bird:set(T_ROT)
    T_SRC.src = WING_SEQ[1]
    scene.bird:set(T_SRC)

    -- Park the pipes off the right edge; they enter once play starts.
    for i = 1, PIPE_N do
        local p = pipes[i]
        p.x = W + 60 + (i - 1) * STRIDE
        p.gap = GAP_MAX
        p.gy = rand_gap_y(GAP_MAX)
        p.scored = false
        place_pipe(p)
    end

    set_txt(scene.score, "0")
    scene.score:clear_flag(lvgl.FLAG.HIDDEN)
    scene.ready1:clear_flag(lvgl.FLAG.HIDDEN)
    scene.ready2:clear_flag(lvgl.FLAG.HIDDEN)
    set_phase(0)
end

enter_dying = function()
    game.state = S_DYING
    game.flash_t = 2
    scene.flash:clear_flag(lvgl.FLAG.HIDDEN)
    if bird.vy < -120 then bird.vy = -120 end
    sfx(game.snd_hit)
    game.new_best = score > 0 and score > best
    if score > best then
        best = score
        save_score(best)
    end
end

enter_over = function()
    game.state = S_OVER
    scene.score:add_flag(lvgl.FLAG.HIDDEN)
    sfx(game.snd_die)

    local cont = make_overlay()

    local go = cont:Image{ src = atlas.text_game_over.src }
    go:set{ align = { type = lvgl.ALIGN.TOP_MID, y_ofs = 14 } }

    local panel = cont:Image{ src = atlas.score.src, x = 155, y = 70 }
    panel:Label{
        text = tostring(score),
        text_font = lvgl.BUILTIN_FONT.MONTSERRAT_22,
        text_color = "#FFFFFF",
        align = { type = lvgl.ALIGN.TOP_LEFT, x_ofs = 15, y_ofs = 25 },
    }
    panel:Label{
        text = tostring(best),
        text_font = lvgl.BUILTIN_FONT.MONTSERRAT_22,
        text_color = "#FFFFFF",
        align = { type = lvgl.ALIGN.BOTTOM_LEFT, x_ofs = 15, y_ofs = -5 },
    }

    if score >= 10 then
        local medal = cont:Image{ src = atlas.medals.src, x = 95, y = 96 }
        local tier
        if score >= 40 then
            tier = "PLATINUM"
            medal:set{ image_recolor = "#E8F4F8", image_recolor_opa = 200 }
        elseif score >= 30 then
            tier = "GOLD"
        elseif score >= 20 then
            tier = "SILVER"
            medal:set{ image_recolor = "#C9CDD4", image_recolor_opa = 200 }
        else
            tier = "BRONZE"
            medal:set{ image_recolor = "#A9642C", image_recolor_opa = 190 }
        end
        cont:Label{
            text = tier,
            text_font = lvgl.BUILTIN_FONT.MONTSERRAT_14,
            text_color = "#FFFFFF",
            x = 117 - #tier * 4, y = 146,
        }
    end

    if game.new_best then
        cont:Label{
            text = "NEW\nBEST!",
            text_font = lvgl.BUILTIN_FONT.MONTSERRAT_14,
            text_color = "#FF5050",
            bg_color = "#000000", bg_opa = 120,
            radius = 4, pad_all = 4,
            x = 20, y = 100,
        }
    end

    local restart = image_button(cont, atlas.button_play, enter_ready)
    restart:set{ align = { type = lvgl.ALIGN.BOTTOM_MID, x_ofs = -45, y_ofs = -2 } }
    local quit = image_button(cont, atlas.button_quit, quit_app)
    quit:set{ align = { type = lvgl.ALIGN.BOTTOM_MID, x_ofs = 55, y_ofs = -6 } }

    arm_overlay(cont)

    -- Slide the whole board in from above; tick_over eases it to 0.
    game.over_y = -H
    T_Y.y = -H
    cont:set(T_Y)
end

-- ── Per-state ticks ─────────────────────────────────────────────────────────
local function tick_menu_ready()
    scroll_world(SPEED0 * DT)
    animate_wings()
    bird.bob = bird.bob + 0.16
    local y = bird.base + floor(sin(bird.bob) * 4 + 0.5)
    if y ~= bird.last_y then
        bird.last_y = y
        T_Y.y = y
        scene.bird:set(T_Y)
    end
end

local function tick_play()
    local dx = world.speed * DT
    scroll_world(dx)
    animate_wings()

    -- Bird physics
    local b = bird
    b.vy = b.vy + GRAVITY * DT
    if b.vy > VY_MAX then b.vy = VY_MAX end
    b.y = b.y + b.vy * DT
    if b.y < CEILING then
        b.y = CEILING
        b.vy = 0
    end
    local yi = floor(b.y)
    if yi ~= b.last_y then
        b.last_y = yi
        T_Y.y = yi
        scene.bird:set(T_Y)
    end

    -- Nose follows the fall; flap() snaps it back up.
    if b.vy > 0 then
        local target = floor(b.vy * 3) - 150
        if target > 900 then target = 900 end
        if target > b.rot then
            b.rot = b.rot + 60
            if b.rot > target then b.rot = target end
            T_ROT.rotation = b.rot
            scene.bird:set(T_ROT)
        end
    end

    if b.y + BIRD_H >= GROUND_Y then
        enter_dying()
        return
    end

    -- Pipes: move, recycle, collide, score
    local bx1 = BIRD_X + HIT_INSET
    local bx2 = BIRD_X + BIRD_W - HIT_INSET
    local by1 = b.y + HIT_INSET
    local by2 = b.y + BIRD_H - HIT_INSET
    for i = 1, PIPE_N do
        local p = pipes[i]
        p.x = p.x - dx
        if p.x < -PIPE_W then
            p.x = p.x + PIPE_N * STRIDE
            p.gap = gap_for(score)
            p.gy = rand_gap_y(p.gap)
            p.scored = false
            place_pipe(p)
        else
            local xi = floor(p.x)
            if xi ~= p.xi then
                p.xi = xi
                T_X.x = xi
                p.top:set(T_X)
                p.bot:set(T_X)
            end
        end
        if bx2 > p.x and bx1 < p.x + PIPE_W then
            if by1 < p.gy or by2 > p.gy + p.gap then
                enter_dying()
                return
            end
        elseif not p.scored and p.x + PIPE_W < bx1 then
            p.scored = true
            add_score()
        end
    end
end

local function tick_dying()
    if game.flash_t > 0 then
        game.flash_t = game.flash_t - 1
        if game.flash_t == 0 then scene.flash:add_flag(lvgl.FLAG.HIDDEN) end
    end

    -- World frozen; the bird tumbles to the ground.
    local b = bird
    b.vy = b.vy + GRAVITY * DT
    if b.vy > VY_MAX then b.vy = VY_MAX end
    b.y = b.y + b.vy * DT
    local floor_y = GROUND_Y - BIRD_H
    local landed = false
    if b.y >= floor_y then
        b.y = floor_y
        landed = true
    end
    local yi = floor(b.y)
    if yi ~= b.last_y then
        b.last_y = yi
        T_Y.y = yi
        scene.bird:set(T_Y)
    end
    if b.rot < 900 then
        b.rot = b.rot + 120
        if b.rot > 900 then b.rot = 900 end
        T_ROT.rotation = b.rot
        scene.bird:set(T_ROT)
    end

    if landed then enter_over() end
end

local function tick_over()
    local oy = game.over_y
    if oy < 0 and scene.overlay then
        oy = oy * 0.6
        if oy > -2 then oy = 0 end
        game.over_y = oy
        T_Y.y = floor(oy)
        scene.overlay:set(T_Y)
    end
end

local function tick()
    if not game.running then return end
    local st = game.state
    if st == S_PLAY then
        tick_play()
    elseif st == S_MENU or st == S_READY then
        tick_menu_ready()
    elseif st == S_DYING then
        tick_dying()
    elseif st == S_OVER then
        tick_over()
    end
end

-- ── Scene construction ──────────────────────────────────────────────────────
local function make_mute_chip(root)
    -- Touch-only on purpose: the chip is in no group, so trackball focus can
    -- never wander onto it mid-round.
    local chip = root:Label{
        text = game.muted and "SFX OFF" or "SFX ON",
        text_font = lvgl.BUILTIN_FONT.MONTSERRAT_14,
        text_color = "#FFFFFF",
        bg_color = "#000000", bg_opa = 120,
        radius = 4, pad_all = 4,
        align = { type = lvgl.ALIGN.TOP_RIGHT, y_ofs = 64, x_ofs = -4 },
    }
    chip:add_flag(lvgl.FLAG.CLICKABLE)
    chip:onevent(lvgl.EVENT.PRESSED, function()
        if not game.running then return end
        game.muted = not game.muted
        save_muted(game.muted)
        if game.muted then
            for _, s in ipairs(game.sounds) do s:stop() end
        end
        set_txt(chip, game.muted and "SFX OFF" or "SFX ON")
    end)
    return chip
end

local function build(root)
    load_sprites(root)
    WING_SEQ = { atlas.bird1.src, atlas.bird2.src, atlas.bird3.src, atlas.bird2.src }

    -- Back-to-front: skies, pipes, land (covers pipe bottoms), bird, flash, UI.
    scene.sky_day, SKY_TILE_W = make_scroll_strip(root, IMAGE_PATH .. "bg_day.png", 0, false)
    scene.sky_night = make_scroll_strip(root, IMAGE_PATH .. "bg_day.png", 0, true)
    scene.sky_night:add_flag(lvgl.FLAG.HIDDEN)

    pipes = {}
    for i = 1, PIPE_N do
        pipes[i] = {
            x = W + 60, gy = 100, gap = GAP_MAX, scored = false, xi = nil,
            -- pipe_up.png's cap is at its bottom edge -> it hangs from the top;
            -- pipe_down.png's cap is at its top edge -> it stands below the gap.
            top = root:Image{ src = atlas.pipe_up.src, x = W + 60, y = 100 - PIPE_H },
            bot = root:Image{ src = atlas.pipe_down.src, x = W + 60, y = 100 + GAP_MAX },
        }
    end

    scene.land, LAND_TILE_W = make_scroll_strip(root, IMAGE_PATH .. "land.png", GROUND_Y, false)

    bird.base = floor(H / 2 - BIRD_H / 2)
    scene.bird = root:Image{ src = atlas.bird1.src, x = BIRD_X, y = bird.base }

    scene.flash = root:Object{
        w = W, h = H, x = 0, y = 0,
        bg_color = "#FFFFFF", bg_opa = 200,
        border_width = 0, pad_all = 0,
    }
    scene.flash:clear_flag(lvgl.FLAG.CLICKABLE)
    scene.flash:clear_flag(lvgl.FLAG.SCROLLABLE)
    scene.flash:add_flag(lvgl.FLAG.HIDDEN)

    local ui = root:Object{ w = W, h = H, x = 0, y = 0, bg_opa = 0, border_width = 0, pad_all = 0 }
    ui:clear_flag(lvgl.FLAG.CLICKABLE)
    ui:clear_flag(lvgl.FLAG.SCROLLABLE)
    scene.ui = ui

    scene.score = ui:Label{
        text = "0",
        text_font = lvgl.BUILTIN_FONT.MONTSERRAT_28,
        text_color = "#FFFFFF",
        bg_color = "#000000", bg_opa = 110,
        radius = 6, pad_all = 5,
        align = { type = lvgl.ALIGN.TOP_MID, y_ofs = 6 },
    }
    scene.score:add_flag(lvgl.FLAG.HIDDEN)

    scene.ready1 = ui:Label{
        text = "GET READY",
        text_font = lvgl.BUILTIN_FONT.MONTSERRAT_28,
        text_color = "#FFFFFF",
        bg_color = "#000000", bg_opa = 120,
        radius = 6, pad_all = 6,
        align = { type = lvgl.ALIGN.CENTER, y_ofs = -34 },
    }
    scene.ready1:add_flag(lvgl.FLAG.HIDDEN)

    scene.ready2 = ui:Label{
        text = "tap or press any key to flap",
        text_font = lvgl.BUILTIN_FONT.MONTSERRAT_14,
        text_color = "#FFFFFF",
        bg_color = "#000000", bg_opa = 120,
        radius = 4, pad_all = 4,
        align = { type = lvgl.ALIGN.CENTER, y_ofs = 4 },
    }
    scene.ready2:add_flag(lvgl.FLAG.HIDDEN)

    scene.mute = make_mute_chip(root)

    -- The root is the tap catcher and key sink; overlay buttons sit above it
    -- and win the hit test. Not CLICK_FOCUSABLE: focus is managed on state
    -- changes, and a background tap during a menu must not steal focus from
    -- the overlay's gridnav.
    root:add_flag(lvgl.FLAG.CLICKABLE)
    root:onevent(lvgl.EVENT.PRESSED, on_tap)
    root:onevent(lvgl.EVENT.KEY, on_key)
    local grp = lvgl.group.get_default()
    grp:add_obj(root)

    print("[flappy] scene ready: " .. #SPRITES .. " sprites, strips "
          .. (W + SKY_TILE_W) .. "x" .. H .. " + " .. (W + LAND_TILE_W) .. "x" .. LAND_H)
end

-- ── Entry ───────────────────────────────────────────────────────────────────
local function entry()
    local root = apps.new_root{
        w = W, h = H,
        bg_color = "#000000", bg_opa = lvgl.OPA(255),
        border_width = 0, pad_all = 0,
    }
    root:clear_flag(lvgl.FLAG.SCROLLABLE)
    scene.root = root

    game.muted = load_muted()
    best = load_score()

    local ok, err = pcall(build, root)
    if not ok then
        -- Degraded screen instead of raising out of the loader.
        print("[flappy] build failed: " .. tostring(err))
        root:Label{
            text = "Flappy Bird failed to start:\n" .. tostring(err),
            text_font = lvgl.BUILTIN_FONT.MONTSERRAT_14,
            text_color = "#FFFFFF",
            align = { type = lvgl.ALIGN.CENTER, y_ofs = -24 },
        }
        local back = root:Label{
            text = "  Back  ",
            text_font = lvgl.BUILTIN_FONT.MONTSERRAT_22,
            text_color = "#FFFFFF",
            bg_color = "#333333", bg_opa = 255,
            radius = 6, pad_all = 8,
            align = { type = lvgl.ALIGN.CENTER, y_ofs = 36 },
        }
        back:add_flag(lvgl.FLAG.CLICKABLE)
        back:add_flag(lvgl.FLAG.CLICK_FOCUSABLE)
        back:onevent(lvgl.EVENT.PRESSED, quit_app)
        _gridnav_add(root, GRIDNAV_ROLLOVER)
        local grp = lvgl.group.get_default()
        grp:add_obj(root)
        lvgl.group.focus_obj(root)
        return
    end

    make_sounds()
    apps.add_timer{ period = TICK_MS, cb = tick }
    enter_menu()
end

entry()

-- Cleanup for EVERY exit path (quit button, home chord, app-to-app launch):
-- go_home runs this before deleting timers and the root, so it must not touch
-- UI. Registered after entry() because apps.new_root clears any earlier
-- callback.
apps.set_on_close(function()
    game.running = false
    for _, s in ipairs(game.sounds) do
        pcall(function() s:delete() end)
    end
    game.sounds = {}
end)
