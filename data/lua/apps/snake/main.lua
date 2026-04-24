local lvgl = require("lvgl")

local app_dir = ...

-- ============================================================
-- T-Deck Snake
-- Two-canvas architecture:
--   board_canvas  – static grid drawn once
--   game_canvas   – redrawn every tick (snake + food)
-- Trackball for direction, click to start/restart
-- ============================================================

local W = lvgl.HOR_RES()
local H = lvgl.VER_RES()

-- Grid / playfield constants
local CELL       = 10                          -- pixels per cell
local BOARD_X    = 10                          -- left margin
local BOARD_Y    = 30                          -- top margin (room for score)
local COLS       = math.floor((W - BOARD_X * 2) / CELL) -- 30
local ROWS       = math.floor((H - BOARD_Y - 10) / CELL) -- 20
local BOARD_W    = COLS * CELL                 -- 300
local BOARD_H    = ROWS * CELL                 -- 200
local TICK_MS    = 150                         -- ms per game tick

-- Colors
local CLR_BG         = "#111111"
local CLR_GRID       = "#1a1a1a"
local CLR_BORDER     = "#444444"
local CLR_SNAKE_HEAD = "#00FF00"
local CLR_SNAKE_BODY = "#00AA00"
local CLR_FOOD       = "#FF2222"
local CLR_TEXT       = "#FFFFFF"
local CLR_DIM_TEXT   = "#888888"

-- ============================================================
-- Central game state (same pattern as flappy bird)
-- ============================================================
local game = {
    running = true,
    playing = false,
    timers  = {},
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
-- Score persistence
-- ============================================================
local function load_score()
    local f = io.open(app_dir .. "/save.txt", "r")
    if f then
        local txt = f:read("*a")
        f:close()
        return tonumber(txt)
    end
    return nil
end

local function save_score(score)
    local f = io.open(app_dir .. "/save.txt", "w")
    if f then
        f:write(tostring(score))
        f:close()
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

local function spawnFood(snake_body)
    -- Pick a random cell not occupied by the snake
    local occupied = {}
    for _, seg in ipairs(snake_body) do
        occupied[seg.x .. "," .. seg.y] = true
    end
    local free = {}
    for x = 0, COLS - 1 do
        for y = 0, ROWS - 1 do
            if not occupied[x .. "," .. y] then
                free[#free + 1] = {x = x, y = y}
            end
        end
    end
    if #free == 0 then return nil end
    return free[math.random(#free)]
end

-- ============================================================
-- Draw the static board (called once)
-- ============================================================
local function drawBoard(canvas)
    -- Background
    canvas:fill_bg(CLR_BG, 255)

    -- Grid lines (vertical)
    for x = 1, COLS - 1 do
        canvas:draw_line({
            p1 = {x = x * CELL, y = 0},
            p2 = {x = x * CELL, y = BOARD_H - 1},
            color = CLR_GRID, width = 1, opa = 255
        })
    end
    -- Grid lines (horizontal)
    for y = 1, ROWS - 1 do
        canvas:draw_line({
            p1 = {x = 0, y = y * CELL},
            p2 = {x = BOARD_W - 1, y = y * CELL},
            color = CLR_GRID, width = 1, opa = 255
        })
    end

    -- Border
    canvas:draw_rect({
        x1 = 0, y1 = 0, x2 = BOARD_W - 1, y2 = BOARD_H - 1,
        bg_opa = 0,
        border_color = CLR_BORDER, border_width = 2, border_opa = 255
    })
end

-- ============================================================
-- Draw game objects (called every tick)
-- ============================================================
local function drawGame(canvas, snake, food)
    -- Clear with full transparency so board shows through
    canvas:fill_bg("#000000", 0)

    -- Draw food
    if food then
        canvas:draw_rect({
            x1 = food.x * CELL + 1, y1 = food.y * CELL + 1,
            x2 = (food.x + 1) * CELL - 2, y2 = (food.y + 1) * CELL - 2,
            bg_color = CLR_FOOD, bg_opa = 255, radius = 2
        })
    end

    -- Draw snake body (tail to head so head draws on top)
    for i = #snake.body, 1, -1 do
        local seg = snake.body[i]
        local color = (i == 1) and CLR_SNAKE_HEAD or CLR_SNAKE_BODY
        canvas:draw_rect({
            x1 = seg.x * CELL + 1, y1 = seg.y * CELL + 1,
            x2 = (seg.x + 1) * CELL - 2, y2 = (seg.y + 1) * CELL - 2,
            bg_color = color, bg_opa = 255, radius = 1
        })
    end
end

-- ============================================================
-- Entry point
-- ============================================================
local function entry()
    local scr = screenCreate()
    game.scr = scr

    local scoreBest = load_score() or 0
    local scoreNow  = 0

    -- Snake state
    local snake = {
        body = {},
        dir  = {x = 1, y = 0},
        next_dir = {x = 1, y = 0},
        grow = 0,
    }
    local food = nil

    local function snakeReset()
        snake.body = {}
        local startX = math.floor(COLS / 2)
        local startY = math.floor(ROWS / 2)
        for i = 0, 2 do
            snake.body[#snake.body + 1] = {x = startX - i, y = startY}
        end
        snake.dir  = {x = 1, y = 0}
        snake.next_dir = {x = 1, y = 0}
        snake.grow = 0
        food = spawnFood(snake.body)
    end

    -- ── Board canvas (static, drawn once) ──
    local board_canvas = scr:Canvas({
        w = BOARD_W, h = BOARD_H,
        x = BOARD_X, y = BOARD_Y,
        bg_opa = 255,
    })
    drawBoard(board_canvas)

    -- ── Game canvas (redrawn every tick, on top of board) ──
    local game_canvas = scr:Canvas({
        w = BOARD_W, h = BOARD_H,
        cf = lvgl.COLOR_FORMAT.ARGB8888,
        x = BOARD_X, y = BOARD_Y,
        bg_opa = 0,
    })

    -- ── Score label ──
    local scoreLabel = scr:Label{
        text = "Score: 0",
        text_font = lvgl.BUILTIN_FONT.MONTSERRAT_14,
        x = BOARD_X, y = 6,
        text_color = CLR_TEXT,
    }

    local bestLabel = scr:Label{
        text = "Best: " .. scoreBest,
        text_font = lvgl.BUILTIN_FONT.MONTSERRAT_14,
        align = { type = lvgl.ALIGN.TOP_RIGHT, x_ofs = -BOARD_X, y_ofs = 6 },
        text_color = CLR_DIM_TEXT,
    }

    -- ── Overlay (menu / game over) ──
    local overlayBox = nil

    local function clearOverlay()
        if overlayBox then overlayBox:delete(); overlayBox = nil end
    end

    local function showOverlay(title, subtitle)
        clearOverlay()
        overlayBox = scr:Object{
            w = 220, h = 60,
            align = { type = lvgl.ALIGN.CENTER },
            bg_color = "#000000", bg_opa = lvgl.OPA(85),
            border_color = CLR_BORDER, border_width = 1,
            radius = 6, pad_all = 8,
        }
        overlayBox:clear_flag(lvgl.FLAG.SCROLLABLE)
        overlayBox:clear_flag(lvgl.FLAG.CLICKABLE)

        overlayBox:Label{
            text = title,
            text_font = lvgl.BUILTIN_FONT.MONTSERRAT_22,
            align = { type = lvgl.ALIGN.TOP_MID, y_ofs = 2 },
            text_color = CLR_TEXT,
        }
        if subtitle then
            overlayBox:Label{
                text = subtitle,
                text_font = lvgl.BUILTIN_FONT.MONTSERRAT_14,
                align = { type = lvgl.ALIGN.BOTTOM_MID, y_ofs = -2 },
                text_color = CLR_DIM_TEXT,
            }
        end
    end

    -- ── Quit button ──
    local quitBtn = scr:Label{
        text = "Quit",
        text_font = lvgl.BUILTIN_FONT.MONTSERRAT_14,
        align = { type = lvgl.ALIGN.BOTTOM_RIGHT, x_ofs = -4, y_ofs = -2 },
        text_color = CLR_DIM_TEXT,
    }
    quitBtn:add_flag(lvgl.FLAG.CLICKABLE)
    quitBtn:onevent(lvgl.EVENT.PRESSED, function()
        game:shutdown()
        local launcher = require("launcher")
        launcher.create()
    end)

    -- ── Input handling ──
    scr:add_flag(lvgl.FLAG.CLICKABLE)
    scr:add_flag(lvgl.FLAG.CLICK_FOCUSABLE)
    local group = lvgl.group.get_default()
    group:add_obj(scr)

    scr:onevent(lvgl.EVENT.KEY, function(obj, code)
        if not game.running then return end
        local indev = lvgl.indev.get_act()
        local key = indev:get_key()

        if key == lvgl.KEY.ENTER then
            if not game.playing then
                -- Start or restart
                snakeReset()
                scoreNow = 0
                scoreLabel:set{ text = "Score: 0" }
                clearOverlay()
                drawGame(game_canvas, snake, food)
                game.playing = true
            end
            return
        end

        if not game.playing then return end

        -- Queue direction change (prevent 180° reversal)
        local dx, dy = snake.dir.x, snake.dir.y
        if key == lvgl.KEY.UP and dy ~= 1 then
            snake.next_dir = {x = 0, y = -1}
        elseif key == lvgl.KEY.DOWN and dy ~= -1 then
            snake.next_dir = {x = 0, y = 1}
        elseif key == lvgl.KEY.LEFT and dx ~= 1 then
            snake.next_dir = {x = -1, y = 0}
        elseif key == lvgl.KEY.RIGHT and dx ~= -1 then
            snake.next_dir = {x = 1, y = 0}
        end
    end)

    -- ── Game tick ──
    local function gameTick()
        if not game.running or not game.playing then return end

        -- Apply queued direction
        snake.dir = snake.next_dir

        -- Compute new head position
        local head = snake.body[1]
        local newHead = {
            x = head.x + snake.dir.x,
            y = head.y + snake.dir.y
        }

        -- Wall collision
        if newHead.x < 0 or newHead.x >= COLS or
           newHead.y < 0 or newHead.y >= ROWS then
            game.playing = false
            if scoreNow > scoreBest then
                scoreBest = scoreNow
                save_score(scoreBest)
                bestLabel:set{ text = "Best: " .. scoreBest }
            end
            showOverlay("Game Over", "Press trackball to restart")
            return
        end

        -- Self collision (check against body, excluding tail if not growing)
        local limit = snake.grow > 0 and #snake.body or #snake.body - 1
        for i = 1, limit do
            if snake.body[i].x == newHead.x and snake.body[i].y == newHead.y then
                game.playing = false
                if scoreNow > scoreBest then
                    scoreBest = scoreNow
                    save_score(scoreBest)
                    bestLabel:set{ text = "Best: " .. scoreBest }
                end
                showOverlay("Game Over", "Press trackball to restart")
                return
            end
        end

        -- Insert new head
        table.insert(snake.body, 1, newHead)

        -- Check food
        if food and newHead.x == food.x and newHead.y == food.y then
            scoreNow = scoreNow + 1
            scoreLabel:set{ text = "Score: " .. scoreNow }
            snake.grow = snake.grow + 1
            food = spawnFood(snake.body)
        end

        -- Remove tail or consume growth
        if snake.grow > 0 then
            snake.grow = snake.grow - 1
        else
            table.remove(snake.body)
        end

        -- Redraw
        drawGame(game_canvas, snake, food)
    end

    local tickTimer = lvgl.Timer{
        period = TICK_MS,
        cb = function(t)
            if not game.running then return end
            gameTick()
        end
    }
    game:trackTimer(tickTimer)

    -- ── Initial state: menu ──
    snakeReset()
    drawGame(game_canvas, snake, food)
    showOverlay("SNAKE", "Press trackball to start")
end

entry()
