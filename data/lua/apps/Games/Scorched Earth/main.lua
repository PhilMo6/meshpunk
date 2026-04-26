local lvgl = require("lvgl")

local app_dir = ...

local W = lvgl.HOR_RES()
local H = lvgl.VER_RES()

local UI_H       = 24
local GAME_Y     = UI_H
local GAME_H     = H - UI_H
local GAME_W     = W

local TANK_W     = 18
local TANK_H     = 8
local BARREL_LEN = 12
local PROJ_R     = 3

local GRAVITY    = 0.12
local MAX_SPEED  = 7
local TICK_MS    = 20

local EXP_RADIUS      = 18
local DIRECT_HIT_R    = 12
local NEAR_HIT_R      = 25
local DIRECT_HIT_DMG  = 40
local NEAR_HIT_DMG    = 15
local EXP_FRAMES      = 6
local EXP_FRAME_MS    = 30

local CLR_SKY        = "#0a0a2e"
local CLR_TERRAIN    = "#4a7a2e"
local CLR_TERRAIN_D  = "#3a6a1e"
local CLR_P1_TANK    = "#2288ff"
local CLR_P1_BARREL  = "#66bbff"
local CLR_AI_TANK    = "#ff4444"
local CLR_AI_BARREL  = "#ff8888"
local CLR_PROJ       = "#ffffff"
local CLR_EXP_INNER  = "#ffff00"
local CLR_EXP_MID    = "#ff8800"
local CLR_EXP_OUTER  = "#ff4400"
local CLR_TEXT       = "#ffffff"
local CLR_DIM        = "#888888"
local CLR_HP_OK      = "#00cc00"
local CLR_HP_WARN    = "#cccc00"
local CLR_HP_CRIT    = "#cc0000"
local CLR_UI_BG      = "#111122"
local CLR_MENU_BG    = "#0d0d24"
local CLR_BTN        = "#334466"

-- ============================================================
-- Game state
-- ============================================================
local game = {
    running = true,
    playing = false,
    timers  = {},
    state   = "menu",
}

function game:alive() return self.running end

function game:trackTimer(t)
    if t then table.insert(self.timers, t) end
    return t
end

function game:shutdown()
    if not self.running then return end
    self.running = false
    self.playing = false

    for _, t in ipairs(self.timers) do
        pcall(function() if t.delete then t:delete() end end)
    end
    self.timers = {}

    if self.scr then
        pcall(function() self.scr:delete() end)
        self.scr = nil
    end
end

-- ============================================================
-- Helpers
-- ============================================================
local function screenCreate(parent)
    local scr = lvgl.Object(parent, {
        w = W, h = H,
        bg_opa = lvgl.OPA(0),
        border_width = 0, pad_all = 0
    })
    scr:clear_flag(lvgl.FLAG.SCROLLABLE)
    scr:clear_flag(lvgl.FLAG.CLICKABLE)
    return scr
end

local function clamp(v, lo, hi)
    if v < lo then return lo end
    if v > hi then return hi end
    return v
end

local function dist(x1, y1, x2, y2)
    local dx = x1 - x2
    local dy = y1 - y2
    return math.sqrt(dx * dx + dy * dy)
end

local function hpColor(hp)
    if hp > 50 then return CLR_HP_OK end
    if hp > 25 then return CLR_HP_WARN end
    return CLR_HP_CRIT
end

-- ============================================================
-- Terrain generation
-- ============================================================
local terrain = {}

local function generateTerrain()
    local base = math.floor(GAME_H * 0.55)
    local s1 = math.random() * 6.28
    local s2 = math.random() * 6.28
    local s3 = math.random() * 6.28
    for x = 0, GAME_W - 1 do
        local h = base
            + math.floor(25 * math.sin(x * 0.02 + s1))
            + math.floor(18 * math.sin(x * 0.04 + s2))
            + math.floor(10 * math.sin(x * 0.09 + s3))
        terrain[x] = clamp(h, 40, GAME_H - 10)
    end
end

-- ============================================================
-- Players
-- ============================================================
local player = { x = 0, y = 0, hp = 100, angle = 60,  power = 50 }
local ai     = { x = 0, y = 0, hp = 100, angle = 120, power = 50 }

local function tankCenterX(t) return t.x end
local function tankCenterY(t) return t.y - math.floor(TANK_H / 2) end

local function placeTanks()
    player.x = 50 + math.random(0, 20)
    player.y = terrain[player.x]
    player.hp = 100
    player.angle = 60
    player.power = 50

    ai.x = GAME_W - 50 - math.random(0, 20)
    ai.y = terrain[ai.x]
    ai.hp = 100
    ai.angle = 120
    ai.power = 50
end

local function sinkTanks()
    player.y = terrain[clamp(player.x, 0, GAME_W - 1)]
    ai.y     = terrain[clamp(ai.x, 0, GAME_W - 1)]
end

-- ============================================================
-- Drawing: terrain canvas
-- ============================================================
local terrain_canvas = nil

local function drawTerrainCanvas()
    if not terrain_canvas then return end
    terrain_canvas:fill_bg(CLR_SKY, 255)

    for x = 0, GAME_W - 1 do
        local ty = terrain[x]
        if ty < GAME_H then
            terrain_canvas:draw_rect({
                x1 = x, y1 = ty,
                x2 = x, y2 = GAME_H - 1,
                bg_color = CLR_TERRAIN, bg_opa = 255
            })
            if ty + 3 < GAME_H then
                terrain_canvas:draw_rect({
                    x1 = x, y1 = ty,
                    x2 = x, y2 = math.min(ty + 3, GAME_H - 1),
                    bg_color = CLR_TERRAIN_D, bg_opa = 255
                })
            end
        end
    end

    local function drawTank(t, bodyClr, barrelClr)
        local tx = t.x - math.floor(TANK_W / 2)
        local ty = t.y - TANK_H
        terrain_canvas:draw_rect({
            x1 = tx, y1 = ty,
            x2 = tx + TANK_W - 1, y2 = t.y - 1,
            bg_color = bodyClr, bg_opa = 255, radius = 2
        })

        local cx = t.x
        local cy = tankCenterY(t)
        local rad = math.rad(t.angle)
        local ex = cx + math.floor(BARREL_LEN * math.cos(rad))
        local ey = cy - math.floor(BARREL_LEN * math.sin(rad))
        terrain_canvas:draw_line({
            p1 = { x = cx, y = cy },
            p2 = { x = ex, y = ey },
            color = barrelClr, width = 3, opa = 255
        })
    end

    drawTank(player, CLR_P1_TANK, CLR_P1_BARREL)
    drawTank(ai, CLR_AI_TANK, CLR_AI_BARREL)
end

-- ============================================================
-- Drawing: projectile canvas
-- ============================================================
local proj_canvas = nil

local function clearProjCanvas()
    if not proj_canvas then return end
    proj_canvas:fill_bg("#000000", 0)
end

local function drawProjectile(px, py)
    if not proj_canvas then return end
    clearProjCanvas()
    proj_canvas:draw_rect({
        x1 = math.floor(px) - PROJ_R, y1 = math.floor(py) - PROJ_R,
        x2 = math.floor(px) + PROJ_R, y2 = math.floor(py) + PROJ_R,
        bg_color = CLR_PROJ, bg_opa = 255, radius = PROJ_R
    })
end

local function drawExplosionFrame(ex, ey, r)
    if not proj_canvas then return end
    clearProjCanvas()
    if r > 4 then
        proj_canvas:draw_rect({
            x1 = ex - r, y1 = ey - r, x2 = ex + r, y2 = ey + r,
            bg_color = CLR_EXP_OUTER, bg_opa = 180, radius = r
        })
    end
    local mr = math.max(1, math.floor(r * 0.65))
    proj_canvas:draw_rect({
        x1 = ex - mr, y1 = ey - mr, x2 = ex + mr, y2 = ey + mr,
        bg_color = CLR_EXP_MID, bg_opa = 220, radius = mr
    })
    local ir = math.max(1, math.floor(r * 0.35))
    proj_canvas:draw_rect({
        x1 = ex - ir, y1 = ey - ir, x2 = ex + ir, y2 = ey + ir,
        bg_color = CLR_EXP_INNER, bg_opa = 255, radius = ir
    })
end

-- ============================================================
-- UI labels
-- ============================================================
local ui = {}

local function createUI(scr)
    scr:Object({
        w = W, h = UI_H, x = 0, y = 0,
        bg_color = CLR_UI_BG, bg_opa = 255,
        border_width = 0, pad_all = 0
    }):clear_flag(lvgl.FLAG.SCROLLABLE)

    ui.p1_hp = scr:Label{
        text = "P1:100",
        text_font = lvgl.BUILTIN_FONT.MONTSERRAT_14,
        x = 5, y = 5,
        text_color = CLR_HP_OK,
    }
    ui.angle = scr:Label{
        text = "ANG:060",
        text_font = lvgl.BUILTIN_FONT.MONTSERRAT_14,
        align = { type = lvgl.ALIGN.TOP_MID, x_ofs = -35, y_ofs = 5 },
        text_color = CLR_TEXT,
    }
    ui.power = scr:Label{
        text = "PWR:050",
        text_font = lvgl.BUILTIN_FONT.MONTSERRAT_14,
        align = { type = lvgl.ALIGN.TOP_MID, x_ofs = 35, y_ofs = 5 },
        text_color = CLR_TEXT,
    }
    ui.ai_hp = scr:Label{
        text = "AI:100",
        text_font = lvgl.BUILTIN_FONT.MONTSERRAT_14,
        align = { type = lvgl.ALIGN.TOP_RIGHT, x_ofs = -5, y_ofs = 5 },
        text_color = CLR_HP_OK,
    }
    ui.turn = scr:Label{
        text = "",
        text_font = lvgl.BUILTIN_FONT.MONTSERRAT_14,
        align = { type = lvgl.ALIGN.TOP_MID, y_ofs = UI_H + 4 },
        text_color = CLR_TEXT,
    }
    ui.turn:add_flag(lvgl.FLAG.HIDDEN)
end

local function updateUI()
    if not game.running then return end
    ui.p1_hp:set{ text = "P1:" .. player.hp, text_color = hpColor(player.hp) }
    ui.ai_hp:set{ text = "AI:" .. ai.hp, text_color = hpColor(ai.hp) }
    ui.angle:set{ text = string.format("ANG:%03d", player.angle) }
    ui.power:set{ text = string.format("PWR:%03d", player.power) }
end

local function showTurnLabel(txt)
    if not game.running then return end
    ui.turn:set{ text = txt }
    ui.turn:clear_flag(lvgl.FLAG.HIDDEN)
    local t = lvgl.Timer{
        period = 800,
        cb = function(timer)
            timer:delete()
            if not game.running then return end
            ui.turn:add_flag(lvgl.FLAG.HIDDEN)
        end
    }
    game:trackTimer(t)
end

-- ============================================================
-- Explosion & damage
-- ============================================================
local currentShooter = nil

local function deformTerrain(ex, ey)
    for x = ex - EXP_RADIUS, ex + EXP_RADIUS do
        if x >= 0 and x < GAME_W then
            local dx = x - ex
            local depth = math.floor(EXP_RADIUS * math.sqrt(math.max(0, 1 - (dx * dx) / (EXP_RADIUS * EXP_RADIUS))))
            local newY = ey + depth
            if newY > terrain[x] then
                terrain[x] = clamp(newY, 0, GAME_H - 1)
            end
        end
    end
end

local function applyDamage(ex, ey)
    local targets = { player, ai }
    for _, t in ipairs(targets) do
        local d = dist(ex, ey, tankCenterX(t), tankCenterY(t))
        if d <= DIRECT_HIT_R then
            t.hp = math.max(0, t.hp - DIRECT_HIT_DMG)
        elseif d <= NEAR_HIT_R then
            t.hp = math.max(0, t.hp - NEAR_HIT_DMG)
        end
    end
end

local function checkWin()
    if player.hp <= 0 then return "ai" end
    if ai.hp <= 0 then return "player" end
    return nil
end

-- Forward declarations
local switchTurn, showGameOver, initGame

local function startExplosion(ex, ey)
    if not game.running then return end
    game.state = "exploding"
    local frame = 0
    local r_step = math.floor(EXP_RADIUS / EXP_FRAMES)

    local t = lvgl.Timer{
        period = EXP_FRAME_MS,
        cb = function(timer)
            if not game.running then timer:delete() return end
            frame = frame + 1
            if frame > EXP_FRAMES then
                timer:delete()
                clearProjCanvas()
                deformTerrain(ex, ey)
                applyDamage(ex, ey)
                sinkTanks()
                drawTerrainCanvas()
                updateUI()

                local winner = checkWin()
                if winner then
                    showGameOver(winner)
                else
                    switchTurn()
                end
                return
            end
            drawExplosionFrame(ex, ey, frame * r_step)
        end
    }
    game:trackTimer(t)
end

-- ============================================================
-- Projectile
-- ============================================================
local proj = { x = 0, y = 0, vx = 0, vy = 0 }

local function fireShot(shooter)
    if not game.running then return end
    currentShooter = shooter
    game.state = "firing"

    local rad = math.rad(shooter.angle)
    local speed = (shooter.power / 100) * MAX_SPEED
    local cx = tankCenterX(shooter)
    local cy = tankCenterY(shooter)
    local sx = cx + math.floor(BARREL_LEN * math.cos(rad))
    local sy = cy - math.floor(BARREL_LEN * math.sin(rad))

    proj.x  = sx
    proj.y  = sy
    proj.vx = speed * math.cos(rad)
    proj.vy = -speed * math.sin(rad)

    local target = (shooter == player) and ai or player

    local t = lvgl.Timer{
        period = TICK_MS,
        cb = function(timer)
            if not game.running then timer:delete() return end
            if game.state ~= "firing" then timer:delete() return end

            proj.x  = proj.x + proj.vx
            proj.y  = proj.y + proj.vy
            proj.vy = proj.vy + GRAVITY

            local ix = clamp(math.floor(proj.x), 0, GAME_W - 1)

            if proj.x < -20 or proj.x > GAME_W + 20 or proj.y > GAME_H + 20 then
                timer:delete()
                clearProjCanvas()
                switchTurn()
                return
            end

            local td = dist(proj.x, proj.y, tankCenterX(target), tankCenterY(target))
            if td <= DIRECT_HIT_R then
                timer:delete()
                startExplosion(math.floor(proj.x), math.floor(proj.y))
                return
            end

            if proj.y >= 0 and proj.y < GAME_H and ix >= 0 and ix < GAME_W then
                if proj.y >= terrain[ix] then
                    timer:delete()
                    startExplosion(ix, terrain[ix])
                    return
                end
            end

            if proj.y >= 0 and proj.y < GAME_H then
                drawProjectile(proj.x, proj.y)
            else
                clearProjCanvas()
            end
        end
    }
    game:trackTimer(t)
end

-- ============================================================
-- AI
-- ============================================================
local function doAITurn()
    if not game.running then return end
    game.state = "ai_turn"
    showTurnLabel("AI TURN")
    updateUI()

    local base_angle
    if player.x < ai.x then
        base_angle = 135
    else
        base_angle = 45
    end
    ai.angle = clamp(base_angle + math.random(-25, 25), 95, 175)
    ai.power = math.random(30, 80)

    drawTerrainCanvas()

    local t = lvgl.Timer{
        period = 1200,
        cb = function(timer)
            timer:delete()
            if not game.running then return end
            fireShot(ai)
        end
    }
    game:trackTimer(t)
end

-- ============================================================
-- Turn management
-- ============================================================
switchTurn = function()
    if not game.running then return end
    if currentShooter == player or currentShooter == nil then
        doAITurn()
    else
        game.state = "player_turn"
        game.playing = true
        showTurnLabel("YOUR TURN")
        updateUI()
    end
end

-- ============================================================
-- Game over overlay
-- ============================================================
local overlayBox = nil

local function clearOverlay()
    if overlayBox then
        pcall(function() overlayBox:delete() end)
        overlayBox = nil
    end
end

showGameOver = function(winner)
    if not game.running then return end
    game.state = "game_over"
    game.playing = false
    clearOverlay()

    local msg = (winner == "player") and "YOU WIN!" or "AI WINS!"

    overlayBox = game.scr:Object{
        w = 200, h = 120,
        align = { type = lvgl.ALIGN.CENTER },
        bg_color = "#000000", bg_opa = lvgl.OPA(90),
        border_color = CLR_DIM, border_width = 1,
        radius = 8, pad_all = 10,
        flex = {
            flex_direction = "column",
            flex_wrap = "nowrap",
            justify_content = "center",
            align_items = "center",
            align_content = "center",
        }
    }
    overlayBox:clear_flag(lvgl.FLAG.SCROLLABLE)

    overlayBox:Label{
        text = msg,
        text_font = lvgl.BUILTIN_FONT.MONTSERRAT_22,
        text_color = CLR_TEXT,
    }

    local playBtn = overlayBox:Object{
        w = 140, h = 30,
        bg_color = CLR_BTN, bg_opa = 255,
        radius = 4, pad_all = 4,
    }
    playBtn:clear_flag(lvgl.FLAG.SCROLLABLE)
    playBtn:add_flag(lvgl.FLAG.CLICKABLE)
    playBtn:Label{
        text = "Play Again",
        text_font = lvgl.BUILTIN_FONT.MONTSERRAT_14,
        align = { type = lvgl.ALIGN.CENTER },
        text_color = CLR_TEXT,
    }

    local exitBtn = overlayBox:Object{
        w = 140, h = 30,
        bg_color = CLR_BTN, bg_opa = 255,
        radius = 4, pad_all = 4,
    }
    exitBtn:clear_flag(lvgl.FLAG.SCROLLABLE)
    exitBtn:add_flag(lvgl.FLAG.CLICKABLE)
    exitBtn:Label{
        text = "Exit",
        text_font = lvgl.BUILTIN_FONT.MONTSERRAT_14,
        align = { type = lvgl.ALIGN.CENTER },
        text_color = CLR_TEXT,
    }

    _gridnav_add(overlayBox, 1)

    playBtn:onevent(lvgl.EVENT.CLICKED, function()
        if not game.running then return end
        clearOverlay()
        generateTerrain()
        placeTanks()
        drawTerrainCanvas()
        clearProjCanvas()
        currentShooter = nil
        game.state = "player_turn"
        game.playing = true
        showTurnLabel("YOUR TURN")
        updateUI()
    end)

    exitBtn:onevent(lvgl.EVENT.CLICKED, function()
        game:shutdown()
        local launcher = require("launcher")
        launcher.create()
    end)
end

-- ============================================================
-- Menu
-- ============================================================
local menuBox = nil

local function clearMenu()
    if menuBox then
        pcall(function() menuBox:delete() end)
        menuBox = nil
    end
end

local function showMenu(scr)
    menuBox = scr:Object{
        w = W, h = H,
        bg_color = CLR_MENU_BG, bg_opa = 255,
        border_width = 0, pad_all = 0,
        flex = {
            flex_direction = "column",
            flex_wrap = "nowrap",
            justify_content = "center",
            align_items = "center",
            align_content = "center",
        }
    }
    menuBox:clear_flag(lvgl.FLAG.SCROLLABLE)

    menuBox:Label{
        text = "SCORCHED",
        text_font = lvgl.BUILTIN_FONT.MONTSERRAT_22,
        text_color = CLR_TEXT,
    }
    menuBox:Label{
        text = "EARTH",
        text_font = lvgl.BUILTIN_FONT.MONTSERRAT_22,
        text_color = CLR_EXP_MID,
    }

    menuBox:Object{ w = 10, h = 20 }:clear_flag(lvgl.FLAG.SCROLLABLE)

    local spBtn = menuBox:Object{
        w = 180, h = 36,
        bg_color = CLR_BTN, bg_opa = 255,
        radius = 6, pad_all = 4,
    }
    spBtn:clear_flag(lvgl.FLAG.SCROLLABLE)
    spBtn:add_flag(lvgl.FLAG.CLICKABLE)
    spBtn:Label{
        text = "Single Player",
        text_font = lvgl.BUILTIN_FONT.MONTSERRAT_14,
        align = { type = lvgl.ALIGN.CENTER },
        text_color = CLR_TEXT,
    }

    menuBox:Object{ w = 10, h = 8 }:clear_flag(lvgl.FLAG.SCROLLABLE)

    local exitBtn = menuBox:Object{
        w = 180, h = 36,
        bg_color = CLR_BTN, bg_opa = 255,
        radius = 6, pad_all = 4,
    }
    exitBtn:clear_flag(lvgl.FLAG.SCROLLABLE)
    exitBtn:add_flag(lvgl.FLAG.CLICKABLE)
    exitBtn:Label{
        text = "Exit",
        text_font = lvgl.BUILTIN_FONT.MONTSERRAT_14,
        align = { type = lvgl.ALIGN.CENTER },
        text_color = CLR_TEXT,
    }

    _gridnav_add(menuBox, 1)

    spBtn:onevent(lvgl.EVENT.CLICKED, function()
        if not game.running then return end
        clearMenu()
        initGame()
    end)

    exitBtn:onevent(lvgl.EVENT.CLICKED, function()
        game:shutdown()
        local launcher = require("launcher")
        launcher.create()
    end)
end

-- ============================================================
-- Game init
-- ============================================================
initGame = function()
    if not game.running then return end

    generateTerrain()
    placeTanks()

    terrain_canvas = game.scr:Canvas({
        w = GAME_W, h = GAME_H,
        x = 0, y = GAME_Y,
        bg_opa = 255,
    })

    proj_canvas = game.scr:Canvas({
        w = GAME_W, h = GAME_H,
        cf = lvgl.COLOR_FORMAT.ARGB8888,
        x = 0, y = GAME_Y,
        bg_opa = 0,
    })

    createUI(game.scr)
    drawTerrainCanvas()
    clearProjCanvas()

    currentShooter = nil
    game.state = "player_turn"
    game.playing = true
    showTurnLabel("YOUR TURN")
    updateUI()
end

-- ============================================================
-- Input
-- ============================================================
local KEY_W = string.byte('w')
local KEY_A = string.byte('a')
local KEY_S = string.byte('s')
local KEY_D = string.byte('d')

local function setupInput(scr)
    scr:add_flag(lvgl.FLAG.CLICKABLE)
    scr:add_flag(lvgl.FLAG.CLICK_FOCUSABLE)
    local group = lvgl.group.get_default()
    group:add_obj(scr)

    scr:onevent(lvgl.EVENT.KEY, function(obj, code)
        if not game.running then return end
        if game.state ~= "player_turn" then return end

        local indev = lvgl.indev.get_act()
        local key = indev:get_key()

        if key == lvgl.KEY.ENTER then
            game.playing = false
            fireShot(player)
            return
        end

        local changed = false

        if key == lvgl.KEY.UP or key == KEY_W then
            player.angle = clamp(player.angle + 2, 1, 179)
            changed = true
        elseif key == lvgl.KEY.DOWN or key == KEY_S then
            player.angle = clamp(player.angle - 2, 1, 179)
            changed = true
        elseif key == lvgl.KEY.LEFT or key == KEY_A then
            player.power = clamp(player.power - 2, 5, 100)
            changed = true
        elseif key == lvgl.KEY.RIGHT or key == KEY_D then
            player.power = clamp(player.power + 2, 5, 100)
            changed = true
        end

        if changed then
            updateUI()
            drawTerrainCanvas()
        end
    end)
end

-- ============================================================
-- Entry point
-- ============================================================
local function entry()
    local scr = screenCreate()
    game.scr = scr
    setupInput(scr)
    showMenu(scr)
end

entry()
