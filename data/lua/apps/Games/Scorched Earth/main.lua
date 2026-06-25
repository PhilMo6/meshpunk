local lvgl = require("lvgl")
local apps = require("lib/apps")

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
local RESULT_MS       = 1800   -- pause showing HIT!/MISS before the turn passes

-- Wind: re-rolled each turn. game.wind is an integer strength in
-- [-WIND_MAX, WIND_MAX]; the per-tick horizontal acceleration applied to a
-- projectile is game.wind * WIND_ACCEL. Positive wind blows to the right.
local WIND_MAX        = 10
local WIND_ACCEL      = 0.0022

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
    mode    = "single",   -- "single" (vs AI) or "two" (local hot-seat)
    wind    = 0,          -- current turn's wind strength
}

function game:alive() return self.running end

function game:trackTimer(t)
    return apps.track_timer(t)   -- manager owns timer teardown
end

function game:shutdown()
    if not self.running then return end
    self.running = false
    self.playing = false
    apps.go_home()   -- manager deletes tracked timers, then the root
end

-- ============================================================
-- Helpers
-- ============================================================
local function screenCreate(parent)
    local scr = apps.new_root({
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

-- The tank whose turn it currently is. In single player this is always `player`
-- when a human is aiming; in two-player both `player` and `ai` are human-driven.
local activeTank = player

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
-- Turn / wind helpers
-- ============================================================
local function newWind()
    game.wind = math.random(-WIND_MAX, WIND_MAX)
end

-- In two-player mode both tanks are controlled by a person; in single player
-- only `player` is.
local function isHuman(t)
    if game.mode == "two" then return true end
    return t == player
end

local function turnName(t)
    if game.mode == "single" then
        return (t == player) and "YOUR TURN" or "AI TURN"
    end
    return (t == player) and "PLAYER 1" or "PLAYER 2"
end

local function focusGame()
    if game.scr then
        pcall(function() lvgl.group.focus_obj(game.scr) end)
    end
end

-- ============================================================
-- Drawing: terrain canvas
-- ============================================================
local terrain_canvas = nil

-- Wind read-out near the top-centre of the play field: "WIND <n>" plus an arrow
-- whose length and direction track the current wind. Drawn onto the terrain
-- canvas (the transparent projectile canvas sits on top, so it stays visible).
local function drawWindIndicator()
    if not terrain_canvas then return end
    local cx  = math.floor(GAME_W / 2)
    local w   = game.wind
    local mag = math.abs(w)

    terrain_canvas:draw_label({
        text = (w == 0) and "WIND --" or ("WIND " .. mag),
        color = CLR_DIM, opa = 255,
        x1 = cx - 30, y1 = 2, x2 = cx + 40, y2 = 16,
    })
    if w == 0 then return end

    local dir = (w > 0) and 1 or -1
    local len = math.min(mag * 4, 40)
    local ay  = 21
    local tip = cx + dir * len
    terrain_canvas:draw_line({
        p1 = { x = cx, y = ay }, p2 = { x = tip, y = ay },
        color = CLR_TEXT, width = 2, opa = 255,
    })
    local hs = 4
    terrain_canvas:draw_triangle({
        p1 = { x = tip,            y = ay },
        p2 = { x = tip - dir * hs, y = ay - hs },
        p3 = { x = tip - dir * hs, y = ay + hs },
        bg_color = CLR_TEXT, bg_opa = 255,
    })
end

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

    drawWindIndicator()
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

    ui.result = scr:Label{
        text = "",
        text_font = lvgl.BUILTIN_FONT.MONTSERRAT_22,
        align = { type = lvgl.ALIGN.CENTER },
        text_color = CLR_TEXT,
    }
    ui.result:add_flag(lvgl.FLAG.HIDDEN)
end

local function updateUI()
    if not game.running then return end
    local me = activeTank or player
    local oppName = (game.mode == "single") and "AI" or "P2"
    ui.p1_hp:set{ text = "P1:" .. player.hp, text_color = hpColor(player.hp) }
    ui.ai_hp:set{ text = oppName .. ":" .. ai.hp, text_color = hpColor(ai.hp) }
    ui.angle:set{ text = string.format("ANG:%03d", me.angle) }
    ui.power:set{ text = string.format("PWR:%03d", me.power) }
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

local function showResult(hit)
    if not game.running or not ui.result then return end
    if hit then
        ui.result:set{ text = "HIT!", text_color = CLR_HP_OK }
    else
        ui.result:set{ text = "MISS", text_color = CLR_DIM }
    end
    ui.result:clear_flag(lvgl.FLAG.HIDDEN)
end

local function hideResult()
    if ui.result then ui.result:add_flag(lvgl.FLAG.HIDDEN) end
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
local switchTurn, showGameOver, initGame, beginTurn, showHandoff, advanceHandoff

-- Hold on the HIT!/MISS read-out for a beat so the round doesn't whip past, then
-- either end the game or hand off to the next turn. Used both after an explosion
-- and after a shot that sails off the screen.
local function resolveTurn(hit)
    if not game.running then return end
    game.state = "resolving"
    showResult(hit)
    local t = lvgl.Timer{
        period = RESULT_MS,
        cb = function(timer)
            timer:delete()
            if not game.running then return end
            hideResult()
            local winner = checkWin()
            if winner then
                showGameOver(winner)
            else
                switchTurn()
            end
        end
    }
    game:trackTimer(t)
end

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

                -- A "hit" means the shooter damaged their opponent.
                local target = (currentShooter == player) and ai or player
                local hpBefore = target.hp
                applyDamage(ex, ey)
                sinkTanks()
                drawTerrainCanvas()
                updateUI()

                resolveTurn(target.hp < hpBefore)
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
            proj.vx = proj.vx + game.wind * WIND_ACCEL

            local ix = clamp(math.floor(proj.x), 0, GAME_W - 1)

            if proj.x < -20 or proj.x > GAME_W + 20 or proj.y > GAME_H + 20 then
                timer:delete()
                clearProjCanvas()
                resolveTurn(false)   -- flew off the map: a miss
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

    -- The AI fires leftward at the player. Wind blowing right (positive) pushes
    -- the shell back toward the AI, so it needs more power; a left wind helps.
    ai.power = clamp(ai.power + math.floor(game.wind * 1.2), 25, 95)

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
-- Roll a fresh wind, then hand the turn to `tank`. A human tank gets an aiming
-- turn; the AI fires on its own.
beginTurn = function(tank)
    if not game.running then return end
    activeTank = tank
    newWind()
    if isHuman(tank) then
        game.state = "player_turn"
        game.playing = true
        focusGame()
        drawTerrainCanvas()
        updateUI()
        showTurnLabel(turnName(tank))
    else
        doAITurn()   -- sets its own aim, redraws, schedules the shot
    end
end

switchTurn = function()
    if not game.running then return end
    -- Whoever just shot is currentShooter; the other tank is up next.
    local nextTank = (currentShooter == ai) and player or ai
    if game.mode == "two" then
        showHandoff(nextTank)   -- explicit "pass the device" gate
    else
        beginTurn(nextTank)
    end
end

-- ============================================================
-- Two-player handoff ("pass the device") gate
-- ============================================================
local handoffBox  = nil
local pendingTank = nil

local function clearHandoff()
    if handoffBox then
        pcall(function() handoffBox:delete() end)
        handoffBox = nil
    end
end

advanceHandoff = function()
    if not game.running then return end
    if game.state ~= "handoff" then return end
    clearHandoff()
    focusGame()
    beginTurn(pendingTank)
end

-- Full-screen prompt shown between turns in two-player mode so players can hand
-- the device over deliberately. Dismissed by a tap or ENTER. It deliberately
-- does NOT join the nav group/gridnav: the screen keeps keyboard focus so its
-- KEY handler sees ENTER, and the box is CLICKABLE so a tap anywhere advances.
showHandoff = function(tank)
    if not game.running then return end
    game.state = "handoff"
    game.playing = false
    pendingTank = tank
    clearHandoff()

    local name = (tank == player) and "PLAYER 1" or "PLAYER 2"
    local clr  = (tank == player) and CLR_P1_TANK or CLR_AI_TANK

    handoffBox = game.scr:Object{
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
    handoffBox:clear_flag(lvgl.FLAG.SCROLLABLE)
    handoffBox:add_flag(lvgl.FLAG.CLICKABLE)

    handoffBox:Label{
        text = "PASS DEVICE TO",
        text_font = lvgl.BUILTIN_FONT.MONTSERRAT_14,
        text_color = CLR_DIM,
    }
    handoffBox:Label{
        text = name,
        text_font = lvgl.BUILTIN_FONT.MONTSERRAT_22,
        text_color = clr,
    }
    handoffBox:Object{ w = 10, h = 16 }:clear_flag(lvgl.FLAG.SCROLLABLE)
    handoffBox:Label{
        text = "Tap or press ENTER",
        text_font = lvgl.BUILTIN_FONT.MONTSERRAT_14,
        text_color = CLR_TEXT,
    }

    handoffBox:onevent(lvgl.EVENT.CLICKED, function()
        advanceHandoff()
    end)

    focusGame()
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
    clearHandoff()
    clearOverlay()

    local msg
    if game.mode == "single" then
        msg = (winner == "player") and "YOU WIN!" or "AI WINS!"
    else
        msg = (winner == "player") and "PLAYER 1 WINS!" or "PLAYER 2 WINS!"
    end

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

    _gridnav_add(overlayBox, GRIDNAV_ROLLOVER)
    local grp = lvgl.group.get_default()
    grp:add_obj(overlayBox)

    playBtn:onevent(lvgl.EVENT.CLICKED, function()
        if not game.running then return end
        clearOverlay()
        generateTerrain()
        placeTanks()
        clearProjCanvas()
        currentShooter = nil
        beginTurn(player)   -- same mode, P1 starts; rolls wind + redraws
    end)

    exitBtn:onevent(lvgl.EVENT.CLICKED, function()
        game:shutdown()   -- ends with apps.go_home()
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
        border_width = 0, pad_all = 0, pad_row = 12,
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

    local mpBtn = menuBox:Object{
        w = 180, h = 36,
        bg_color = CLR_BTN, bg_opa = 255,
        radius = 6, pad_all = 4,
    }
    mpBtn:clear_flag(lvgl.FLAG.SCROLLABLE)
    mpBtn:add_flag(lvgl.FLAG.CLICKABLE)
    mpBtn:Label{
        text = "2 Players (Local)",
        text_font = lvgl.BUILTIN_FONT.MONTSERRAT_14,
        align = { type = lvgl.ALIGN.CENTER },
        text_color = CLR_TEXT,
    }

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

    _gridnav_add(menuBox, GRIDNAV_ROLLOVER)
    local grp = lvgl.group.get_default()
    grp:add_obj(menuBox)

    spBtn:onevent(lvgl.EVENT.CLICKED, function()
        if not game.running then return end
        game.mode = "single"
        clearMenu()
        initGame()
    end)

    mpBtn:onevent(lvgl.EVENT.CLICKED, function()
        if not game.running then return end
        game.mode = "two"
        clearMenu()
        initGame()
    end)

    exitBtn:onevent(lvgl.EVENT.CLICKED, function()
        game:shutdown()   -- ends with apps.go_home()
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
    clearProjCanvas()

    currentShooter = nil
    beginTurn(player)   -- P1 starts in both modes; rolls wind + draws
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
    lvgl.group.focus_obj(scr)

    scr:onevent(lvgl.EVENT.KEY, function(obj, code)
        if not game.running then return end

        local indev = lvgl.indev.get_act()
        local key = indev:get_key()

        -- Between-turn handoff in two-player mode: ENTER advances.
        if game.state == "handoff" then
            if key == lvgl.KEY.ENTER then advanceHandoff() end
            return
        end

        if game.state ~= "player_turn" then return end

        local me = activeTank or player

        if key == lvgl.KEY.ENTER then
            game.playing = false
            fireShot(me)
            return
        end

        local changed = false

        if key == lvgl.KEY.UP or key == KEY_W then
            me.angle = clamp(me.angle + 2, 1, 179)
            changed = true
        elseif key == lvgl.KEY.DOWN or key == KEY_S then
            me.angle = clamp(me.angle - 2, 1, 179)
            changed = true
        elseif key == lvgl.KEY.LEFT or key == KEY_A then
            me.power = clamp(me.power - 2, 5, 100)
            changed = true
        elseif key == lvgl.KEY.RIGHT or key == KEY_D then
            me.power = clamp(me.power + 2, 5, 100)
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
    local scr = screenCreate()   -- apps.new_root inside: already registered
    game.scr = scr
    setupInput(scr)
    showMenu(scr)
end

entry()
