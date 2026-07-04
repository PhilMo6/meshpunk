local lvgl = require("lvgl")
local sound = require("lib/sound")
local apps = require("lib/apps")

-- Receive app directory from launcher (e.g. "L:/lua/apps/flappyBird")
local app_dir = ...

-- ============================================================
-- T-Deck Flappy Bird
-- All images loaded as PNGs from LittleFS via LVGL FS driver
-- LVGL now uses system malloc -> PSRAM for large allocations
-- ============================================================

local W = lvgl.HOR_RES()
local H = lvgl.VER_RES()

-- Game constants tuned for T-Deck 320x240
local LAND_HEIGHT = 56
local MOVE_SPEED = W / 8000
local PIXEL_PER_METER = math.floor(H / 6)
local TOP_Y = 10
local BOTTOM_Y = H - LAND_HEIGHT
local PIPE_COUNT = math.max(3, math.floor(W / 80))
local PIPE_GAP = 55
local PIPE_SPACE = math.floor(W / 3)

local IMAGE_PATH = app_dir .. "/"

-- ============================================================
-- Central game state
--   running : app is alive. Set false on shutdown BEFORE any widget
--             is deleted, so in-flight anim/timer ticks short-circuit.
--   playing : a round is currently in progress (bird flying, pipes moving).
--   anims/timers: registries used by shutdown() to stop everything
--             without the exit path needing to know which objects exist.
-- ============================================================
local game = {
    running = true,
    playing = false,
    anims = {},
    timers = {},
}

function game:alive() return self.running end

function game:trackAnim(a)
    if a then table.insert(self.anims, a) end
    return a
end

function game:trackTimer(t)
    return apps.track_timer(t)   -- manager owns timer teardown
end

function game:shutdown()
    if not self.running then return end
    self.running = false
    self.playing = false

    for _, a in ipairs(self.anims) do
        pcall(function() if a.stop then a:stop() end end)
    end
    self.anims = {}

    if self.gameover_snd then pcall(function() self.gameover_snd:delete() end); self.gameover_snd = nil end
    if self.flap_snd then pcall(function() self.flap_snd:delete() end); self.flap_snd = nil end

    apps.go_home()   -- manager deletes tracked timers, then the root
end

-- Try to load best score from file
local function load_score()
    local f = io.open(app_dir .. "/save.txt", "r")
    if f then
        local txt = f:read("*a")
        f:close()
        return tonumber(txt)
    else
        print("No existing save found")
        return
    end
end

-- Save current best score to file
local function save_score(score)
    local f = io.open(app_dir .. "/save.txt", "w")
    if f then
        f:write(score)
        f:close()
    else
        print("Failed to write save file")
    end
end

local function randomY()
    return math.random(TOP_Y + 20, BOTTOM_Y - PIPE_GAP - 20)
end

-- One managed root (apps.new_root) for the whole app; every layer after that
-- is a plain child Object. Calling new_root per layer (the old behavior)
-- repointed M._screen and RESET M._timers each time — go_home then deleted
-- only the LAST layer, leaving the sky/land/bird layers orphaned on the
-- screen (the "ground stays over the launcher" bug) with their infinite
-- scroll anims and the lost wing timer still running (the permanent lag).
local function screenCreate(parent)
    local props = {
        w = W, h = H,
        bg_opa = lvgl.OPA(0),
        border_width = 0, pad_all = 0
    }
    local scr
    if parent then
        scr = parent:Object(props)
        scr:set{ x = 0, y = 0 }
    else
        scr = apps.new_root(props)
    end
    scr:clear_flag(lvgl.FLAG.SCROLLABLE)
    scr:clear_flag(lvgl.FLAG.CLICKABLE)
    return scr
end

local function Image(parent, src)
    local img = {}
    img.widget = parent:Image{ src = src }
    img.w, img.h = img.widget:get_img_size()
    if not img.w or not img.h then
        error("failed to load image: " .. tostring(src))
    end
    return img
end

-- Endless horizontal scroller. The PNG is decoded ONCE into two app-owned
-- Canvas buffers (same pattern as the pipes) and the canvases slide; drawing
-- a canvas is a direct blit of its own pixels. The old version slid two Image
-- widgets, which re-render from the decoder cache every frame — full-screen
-- layers overflow LV_CACHE_DEF_SIZE, and a full cache makes LVGL FAIL the
-- decode: invisible background + per-frame re-decode churn (the lag).
local function ImageScroll(root, src, animSpeed, y)
    local probe = Image(root, src)   -- just to read the image dimensions
    local iw, ih = probe.w, probe.h
    probe.widget:delete()

    local function makeStrip(x)
        local c = root:Canvas{
            w = iw, h = ih,
            cf = lvgl.COLOR_FORMAT.ARGB8888,
            x = x, y = y
        }
        c:fill_bg("#000000", 0)
        c:draw_image{ x1 = 0, y1 = 0, x2 = iw - 1, y2 = ih - 1,
                      src = src, opa = 255 }
        c:clear_flag(lvgl.FLAG.CLICKABLE)
        return c
    end

    local img = makeStrip(0)
    local right = makeStrip(W)

    local anim = img:Anim{
        run = true,
        start_value = 0,
        end_value = -W,
        time = W / animSpeed,
        repeat_count = lvgl.ANIM_REPEAT_INFINITE,
        path = "linear",
        exec_cb = function(obj, value)
            if not game.running then return end
            img:set{ x = value }
            right:set{ x = value + W }
        end
    }
    game:trackAnim(anim)

    return {
        widget = img,
        anim = anim,
        stop = function() if anim and anim.stop then anim:stop() end end
    }
end

local function Frames(parent, srcs, fps)
    local frame = Image(parent, srcs[1])
    fps = fps ~= 0 and fps or 25
    frame.src = srcs
    frame.len = #srcs
    frame.i = 0

    frame.timer = lvgl.Timer {
        period = 1000 / fps,
        cb = function(t)
            if not game.running then return end
            frame.widget:set{ src = frame.src[frame.i] }
            frame.i = frame.i + 1
            if frame.i == frame.len then frame.i = 1 end
        end
    }
    game:trackTimer(frame.timer)

    frame.start = function(self) self.timer:resume() end
    frame.pause = function(self) self.timer:pause() end
    return frame
end

local function ObjInfo(x, y, w, h)
    return { x = x, y = y, w = w, h = h }
end

local function Pipes(parent)
    local pipes = {}

    -- Get pipe image dimensions via a temporary widget
    local tmp = Image(parent, IMAGE_PATH .. "pipe_up.png")
    local pipe_w = tmp.w
    local pipe_h = tmp.h
    tmp.widget:delete()

    pipes.w = pipe_w
    pipes.h = pipe_h

    local stride   = PIPE_SPACE + pipe_w
    local canvas_w = PIPE_COUNT * stride + pipe_w
    local canvas_h = BOTTOM_Y

    pipes.canvas = parent:Canvas{
        w = canvas_w, h = canvas_h,
        cf = lvgl.COLOR_FORMAT.ARGB8888,
        x = W, y = 0
    }
    pipes.canvas:clear_flag(lvgl.FLAG.CLICKABLE)

    for i = 1, PIPE_COUNT do
        pipes[i] = { canvas_x = (i - 1) * stride, y = randomY(), x = (i - 1) * stride + W }
    end

    pipes.birdInfo = ObjInfo(0, 0, 0, 0)
    pipes.gapInfo  = ObjInfo(0, 0, 0, 0)

    local function drawPipes()
        pipes.canvas:fill_bg("#000000", 0)
        for i = 1, PIPE_COUNT do
            local p  = pipes[i]
            local cx = p.canvas_x
            pipes.canvas:draw_image{
                x1 = cx, y1 = p.y - pipe_h,
                x2 = cx + pipe_w - 1, y2 = p.y - 1,
                src = IMAGE_PATH .. "pipe_up.png", opa = 255
            }
            pipes.canvas:draw_image{
                x1 = cx, y1 = p.y + PIPE_GAP,
                x2 = cx + pipe_w - 1, y2 = p.y + PIPE_GAP + pipe_h - 1,
                src = IMAGE_PATH .. "pipe_down.png", opa = 255
            }
        end
    end

    local function pipesPosinit()
        for i = 1, PIPE_COUNT do
            pipes[i].canvas_x = (i - 1) * stride
            pipes[i].y        = randomY()
            pipes[i].x        = (i - 1) * stride + W
        end
        pipes.scroll_offset   = 0
        pipes.canvas_widget_x = W
        pipes.front           = 1
        pipes.last            = PIPE_COUNT
        pipes.canvas:set{ x = W }
        drawPipes()
    end

    pipesPosinit()

    pipes.score      = 0
    pipes.objPassing = -1

    function pipes:setObjInfo(x, y, w, h)
        self.birdInfo.x = x
        self.birdInfo.y = y
        if w then self.birdInfo.w = w end
        if h then self.birdInfo.h = h end
    end

    local function setGapInfo(x, y, w, h)
        pipes.gapInfo.x = x
        pipes.gapInfo.y = y
        pipes.gapInfo.w = w
        pipes.gapInfo.h = h
    end

    local function isBirdCollision()
        local bird = pipes.birdInfo
        local gap  = pipes.gapInfo
        if bird.x + bird.w < gap.x then return false end
        if bird.x > gap.x + gap.w  then return false end
        if (bird.y > gap.y) and (bird.y + bird.h < gap.y + gap.h) then return false end
        return true
    end

    local function checkScore(i)
        local bird    = pipes.birdInfo
        local gap     = pipes.gapInfo
        local passing = pipes.objPassing
        if bird.x + bird.w < gap.x or bird.x > gap.x + gap.w then
            if passing > 0 and i == passing then
                pipes.score = pipes.score + 1
                passing = -1
                pipes.scoreUpdateCB(pipes.score)
            end
        else
            if passing < 0 then passing = i end
        end
        pipes.objPassing = passing
    end

    local function collisionDetect()
        local first = (pipes.last % PIPE_COUNT) + 1
        for idx = 0, PIPE_COUNT - 1 do
            local i    = (first + idx - 1) % PIPE_COUNT + 1
            local pipe = pipes[i]
            setGapInfo(pipe.x, pipe.y, pipes.w, PIPE_GAP)
            if isBirdCollision() then
                if pipes.collisionCB then pipes.collisionCB() end
            end
            checkScore(i)
        end
    end

    pipes.preValue = 0
    pipes.anim = pipes.canvas:Anim{
        run = false,
        start_value = 0,
        end_value = W,
        time = W / MOVE_SPEED,
        repeat_count = lvgl.ANIM_REPEAT_INFINITE,
        path = "linear",
        exec_cb = function(obj, value)
            if not game.running or not game.playing then return end
            local x = pipes.preValue
            local d
            if value < x then d = value + W - x else d = value - x end
            pipes.preValue = value

            pipes.scroll_offset   = pipes.scroll_offset + d
            pipes.canvas_widget_x = W - pipes.scroll_offset
            pipes.canvas:set{ x = pipes.canvas_widget_x }

            for i = 1, PIPE_COUNT do
                pipes[i].x = pipes[i].canvas_x + pipes.canvas_widget_x
            end

            local front_pipe = pipes[pipes.front]
            if front_pipe.canvas_x + pipes.canvas_widget_x + pipe_w < 0 then
                local prev_idx      = (pipes.front - 2 + PIPE_COUNT) % PIPE_COUNT + 1
                front_pipe.canvas_x = pipes[prev_idx].canvas_x + stride
                front_pipe.y        = randomY()
                pipes.last          = pipes.front
                pipes.front         = pipes.front % PIPE_COUNT + 1
                -- Shift all canvas_x left by stride to keep within canvas bounds
                for i = 1, PIPE_COUNT do
                    pipes[i].canvas_x = pipes[i].canvas_x - stride
                end
                pipes.scroll_offset   = pipes.scroll_offset - stride
                pipes.canvas_widget_x = W - pipes.scroll_offset
                -- Recompute screen positions after the shift
                for i = 1, PIPE_COUNT do
                    pipes[i].x = pipes[i].canvas_x + pipes.canvas_widget_x
                end
                drawPipes()
                pipes.canvas:set{ x = pipes.canvas_widget_x }
            end

            collisionDetect()
        end
    }
    game:trackAnim(pipes.anim)

    function pipes:start() self.anim:start() end
    function pipes:stop()  self.anim:stop()  end
    function pipes:reset()
        pipesPosinit()
        pipes.score      = 0
        pipes.preValue   = 0
        pipes.objPassing = -1
    end
    function pipes:setCollisionCB(cb)    self.collisionCB    = cb end
    function pipes:setScoreUpdateCB(cb) self.scoreUpdateCB = cb end

    return pipes
end

local function Bird(parent, birdMovedCB)
    local bird = Frames(parent,
        {IMAGE_PATH .. "bird1.png", IMAGE_PATH .. "bird2.png", IMAGE_PATH .. "bird3.png"}, 5)

    local function birdVarInit()
        bird.x = math.floor(W / 3 - bird.w / 2)
        bird.y = math.floor(H / 2 - bird.h / 2)
        bird.widget:set{ x = bird.x, y = bird.y }
        bird.head = 0
        bird.force = 0
        bird.velocity = 0
        bird.time = 0
        bird.moving = false
    end

    birdVarInit()

    bird.setY = function(self) bird.widget:set{ y = bird.y } end
    bird.setHead = function(self) bird.widget:set{ rotation = self.head } end

    bird.applyForce = function(self, force)
        self.force = force
        if bird.moving then return end
        bird.moving = true
        self.y_anim:start()
    end

    bird.pressed = function(self) bird:applyForce(-9); bird.velocity = 0 end
    bird.released = function(self) bird:applyForce(5); bird.velocity = 0 end

    local function velocity2HeadAngle(v) return v * 60 end

    bird.y_anim = bird.widget:Anim{
        run = false,
        start_value = 0, end_value = 1000,
        time = 1000,
        repeat_count = lvgl.ANIM_REPEAT_INFINITE,
        path = "linear",
        exec_cb = function(obj, tNow)
            if not game.running then return end
            if tNow < bird.time then tNow = tNow + 1000 end
            local v = bird.velocity
            local t = tNow < bird.time and tNow + 1000 - bird.time or tNow - bird.time
            t = t * 0.001

            v = bird.force * t + v
            if v > 10 then v = 10 end
            if v < -10 then v = -10 end

            local y = bird.y + v * t * PIXEL_PER_METER
            if y > BOTTOM_Y - bird.h then y = BOTTOM_Y - bird.h; v = 0 end
            if y < TOP_Y then y = TOP_Y; v = 0 end

            bird.y = y
            bird.time = tNow
            bird.velocity = v
            bird.head = velocity2HeadAngle(v)

            birdMovedCB(bird.x, bird.y)
            bird:setY()
            bird:setHead()
        end
    }
    game:trackAnim(bird.y_anim)

    function bird:stop() bird.y_anim:stop() end
    function bird:gameOver() bird.released() end
    function bird:start() bird.y_anim:start() end
    function bird:reset() bird.stop(); birdVarInit() end

    return bird
end

local function Background(root, bgEventCB)
    local bgLayer = screenCreate(root)
    bgLayer:add_flag(lvgl.FLAG.CLICKABLE)
    bgLayer:add_flag(lvgl.FLAG.CLICK_FOCUSABLE)
    local group = lvgl.group.get_default()
    group:add_obj(bgLayer)
    lvgl.group.focus_obj(bgLayer)

    local bg = ImageScroll(bgLayer, IMAGE_PATH .. "bg_day.png", MOVE_SPEED * 0.4, 0)
    local pipes = Pipes(bgLayer)
    local land = ImageScroll(bgLayer, IMAGE_PATH .. "land.png", MOVE_SPEED, BOTTOM_Y)

    bgLayer:onevent(lvgl.EVENT.PRESSED, function(obj, code)
        if not game.running then return end
        bgEventCB(lvgl.EVENT.PRESSED)
    end)
    bgLayer:onevent(lvgl.EVENT.RELEASED, function(obj, code)
        if not game.running then return end
        bgEventCB(lvgl.EVENT.RELEASED)
    end)

    return {bgLayer = bgLayer, pipes = pipes, bg = bg, land = land }
end

local function SysLayer(root) return screenCreate(root) end

local function createPlayBtn(sysLayer, onEvent)
    local playBtn = Image(sysLayer, IMAGE_PATH .. "button_play.png").widget
    playBtn:add_flag(lvgl.FLAG.CLICKABLE)
    playBtn:set{ align = { type = lvgl.ALIGN.CENTER, y_ofs = math.floor(H / 6) } }
    playBtn:onevent(lvgl.EVENT.PRESSED, function(obj, code)
        if not game.running then return end
        onEvent(obj, code)
    end)
    return playBtn
end

local function createQuitBtn(sysLayer)
    local quitBtn = Image(sysLayer, IMAGE_PATH .. "button_quit.png").widget
    quitBtn:add_flag(lvgl.FLAG.CLICKABLE)
    quitBtn:set{ align = { type = lvgl.ALIGN.TOP_RIGHT } }
    quitBtn:onevent(lvgl.EVENT.PRESSED, function()
        -- Single exit point: shutdown flips game.running=false FIRST so any
        -- in-flight anim/timer tick short-circuits, then stops and deletes.
        game:shutdown()   -- ends with apps.go_home()
    end)
    return quitBtn
end

local function entry()
    local scr = screenCreate()   -- apps.new_root inside: already registered
    game.scr = scr
    local bird, pipes, sysLayer
    local gameStart, gameOver
    local scoreBest = load_score() or 0
    local scoreNow = 0
    local debouncing = false

    -- Game over: chromatic descent, plays once
    game.gameover_snd = sound.generateMelody({
        {freq=659, ms=250}, {freq=0, ms=30},
        {freq=622, ms=250}, {freq=0, ms=30},
        {freq=587, ms=250}, {freq=0, ms=30},
        {freq=523, ms=400}, {freq=0, ms=50},
        {freq=494, ms=250}, {freq=0, ms=30},
        {freq=440, ms=250}, {freq=0, ms=30},
        {freq=392, ms=400}, {freq=0, ms=50},
        {freq=330, ms=700},
    }, { waveform = "square", attack = 10, decay = 80, sustain = 0.4, release = 100 })

    -- Flap sound: 600ms to match 3-frame @ 5fps wing animation cycle
    game.flap_snd = sound.generateTone(200, 600, {
        end_freq = 100,
        waveform = "triangle",
        attack = 5,
        decay = 150,
        sustain = 0.0,
        release = 50,
    })
    if game.flap_snd then game.flap_snd:setLoop(true) end

    local scoreLabel
    local function createScoreLabel()
        if not scoreLabel then
            scoreLabel = sysLayer:Label{
                text = "000",
                text_font = lvgl.BUILTIN_FONT.MONTSERRAT_28,
                align = { type = lvgl.ALIGN.TOP_LEFT, x_ofs = 0, y_ofs = 50 }
            }
        end
    end
    local scoreUpdateCB = function(score)
        if not game.running then return end
        if scoreLabel then scoreLabel:set{ text = string.format("%03d", score) } end
        scoreNow = score
    end

    gameStart = function()
        if not game.running or game.playing then return end
        bird:reset()
        pipes:reset()
        pipes:start()
        bird:start()
        game.playing = true
        if game.gameover_snd then game.gameover_snd:stop() end
        scoreNow = 0
        createScoreLabel()
    end

    gameOver = function()
        if not game.running or not game.playing then return end
        debouncing = true
        game.playing = false
        if game.flap_snd then game.flap_snd:stop() end
        if game.gameover_snd then game.gameover_snd:play() end
        pipes:stop()
        bird:gameOver()
        if scoreNow > scoreBest then
            scoreBest = scoreNow
            save_score(scoreBest)
        end
        if scoreLabel then scoreLabel:delete() scoreLabel = nil end

        game.gameoverImg = Image(sysLayer, IMAGE_PATH .. "text_game_over.png").widget
        game.gameoverImg:set{ align = { type = lvgl.ALIGN.TOP_MID, y_ofs = math.floor(H * 0.2) } }

        game.gameoverImgAnim = game.gameoverImg:Anim{
            run = true, start_value = 0, end_value = 3600,
            time = 5000, repeat_count = 2, path = "bounce",
            exec_cb = function(obj, value)
                if not game.running then return end
                obj:set{ rotation = value }
            end
        }
        game:trackAnim(game.gameoverImgAnim)

        local scoreImg = Image(sysLayer, IMAGE_PATH .. "score.png").widget
        scoreImg:set{ align = { type = lvgl.ALIGN.TOP_LEFT, y_ofs = 0 } }
        local scoreImgAnim = scoreImg:Anim{
            run = true, start_value = H, end_value = 0,
            time = 1000, repeat_count = 1, path = "ease_in",
            exec_cb = function(obj, value)
                if not game.running then return end
                obj:set{ align = { type = lvgl.ALIGN.TOP_LEFT, x_ofs = 0, y_ofs = value } }
            end
        }
        game:trackAnim(scoreImgAnim)

        scoreImg:Label{
            text = string.format("%03d", scoreNow),
            text_font = lvgl.BUILTIN_FONT.MONTSERRAT_22,
            align = { type = lvgl.ALIGN.TOP_LEFT, x_ofs = 15, y_ofs = 25 }
        }

        scoreImg:Label{
            text = string.format("%03d", scoreBest),
            text_font = lvgl.BUILTIN_FONT.MONTSERRAT_22,
            align = { type = lvgl.ALIGN.BOTTOM_LEFT, x_ofs = 15, y_ofs = -5 }
        }
        scoreNow = 0

        local playBtn
        local quitBtn
        playBtn = createPlayBtn(sysLayer, function(obj, code)
            if debouncing then return end
            gameStart()
            quitBtn:delete(); quitBtn = nil
            playBtn:delete(); playBtn = nil
            game.gameoverImgAnim:delete(); game.gameoverImgAnim = nil
            game.gameoverImg:delete(); game.gameoverImg = nil
            scoreImg:delete(); scoreImg = nil

            createScoreLabel()

            local grp = lvgl.group.get_default()
            grp:add_obj(bgLayer.bgLayer)
            lvgl.group.focus_obj(bgLayer.bgLayer)
        end)

        quitBtn = createQuitBtn(sysLayer)
        _gridnav_add(sysLayer, GRIDNAV_ROLLOVER)
        local grp = lvgl.group.get_default()
        grp:add_obj(sysLayer)

        local debTimer = lvgl.Timer {
            period = 1000,
            cb = function(t)
                t:delete()
                if not game.running then return end
                debouncing = false
            end
        }
        game:trackTimer(debTimer)
    end

    local bgEventCB = function(event)
        if not game.running or not game.playing then return end
        if event == lvgl.EVENT.PRESSED then
            bird:pressed()
            if game.flap_snd then game.flap_snd:play() end
        else
            bird:released()
            if game.flap_snd then game.flap_snd:stop() end
        end
    end

    local birdMovedCB = function(x, y)
        if not game.running then return end
        pipes:setObjInfo(bird.x, bird.y)
    end

    local collisionCB = function()
        if not game.running then return end
        local t = lvgl.Timer { period = 10, cb = function(t)
            t:delete()
            if not game.running then return end
            gameOver()
        end }
        game:trackTimer(t)
    end

    -- background layer (scrolling sky + pipes + scrolling land)
    local bgLayer = Background(scr, bgEventCB)
    game.bgLayer = bgLayer
    pipes = bgLayer.pipes
    game.pipes = pipes
    pipes:setCollisionCB(collisionCB)
    pipes:setScoreUpdateCB(scoreUpdateCB)

    -- main layer (bird)
    local mainLayer = screenCreate(scr)
    bird = Bird(mainLayer, birdMovedCB)
    game.bird = bird
    pipes:setObjInfo(bird.x, bird.y, bird.w, bird.h)

    -- system layer (UI overlays)
    sysLayer = SysLayer(scr)
    game.sysLayer = sysLayer

    local title = Image(sysLayer, IMAGE_PATH .. "title.png").widget
    title:set{ align = { type = lvgl.ALIGN.TOP_MID, y_ofs = math.floor(H * 0.15) } }

    local playBtn
    local quitBtn
    playBtn = createPlayBtn(sysLayer, function()
        quitBtn:delete(); quitBtn = nil
        playBtn:delete(); playBtn = nil
        title:delete(); title = nil

        local medal = Image(sysLayer, IMAGE_PATH .. "medals.png").widget
        medal:set{ align = { type = lvgl.ALIGN.TOP_LEFT, y_ofs = 4, x_ofs = 4 } }
        createScoreLabel()

        local grp = lvgl.group.get_default()
        grp:add_obj(bgLayer.bgLayer)
        lvgl.group.focus_obj(bgLayer.bgLayer)

        gameStart()
    end)

    quitBtn = createQuitBtn(sysLayer)
    _gridnav_add(sysLayer, GRIDNAV_ROLLOVER)
    local grp = lvgl.group.get_default()
    grp:add_obj(sysLayer)
end

entry()
