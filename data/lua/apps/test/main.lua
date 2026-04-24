local app_dir = ...

local W = lvgl.HOR_RES()
local H = lvgl.VER_RES()
local opa = lvgl.OPA(100)

local root = lvgl.Object()
root:set { 
    w = W,
    h = H,
    align = lvgl.ALIGN.CENTER
}

-- create obj on root
local obj = root:Object({
    x = W // 2,
    y = H // 2 + 25,
    w = 20,
    h = 20,
    bg_opa = opa,
    bg_color = "#a95f1f"
})

-- create image on root and set position/img src/etc. properties.
local img = root:Image {
    x = W // 2,
    y = H // 2,
    w = 20,
    h = 20,
    bg_opa = opa,
    bg_color = "#1fa931"
}


    local canvas = root:Canvas({
        w=100, 
        h=100,
        bg_opa = opa
    })
canvas:fill_bg("#73ea83", 255)
canvas:draw_rect({x1=10, y1=10, x2=90, y2=90, bg_color="#FF0000", bg_opa=255, radius=5})
canvas:draw_line({p1={x=0, y=0}, p2={x=99, y=99}, color="#00FF00", width=2})
canvas:draw_arc({center={x=50, y=50}, radius=30, start_angle=0, end_angle=270, color="#0000FF", width=3})
canvas:draw_label({x1=10, y1=40, x2=90, y2=60, text="Hi", color="#FFFFFF"})