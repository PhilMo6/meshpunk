-- Utility functions for Lua apps

local W = lvgl.HOR_RES()
local H = lvgl.VER_RES()

local utils = {}

-- Format time string
function utils.formatTime(timestamp)
    local time = os.date("*t", timestamp or os.time())
    return string.format("%02d:%02d", time.hour, time.min)
end

-- Create a simple notification
function utils.createNotification(parent, message, duration)
    duration = duration or 3000  -- default 3 seconds
    
    local notification = parent:Object {
        bg_color = "#333333", 
        radius = 8,
        border_width = 0,
        pad_all = 10,
        w = 280,
        h = lvgl.SIZE_CONTENT,
        align = lvgl.ALIGN.BOTTOM_MID,
        y = -20
    }
    
    notification:Label {
        text = message,
        text_color = "#FFFFFF",
        align = lvgl.ALIGN.CENTER,
    }
    
    -- Animate in from bottom
    notification:set { y = 50 }
    notification:Anim {
        run = true,
        start_value = 50,
        end_value = -20,
        duration = 300,
        path = "ease_out",
        exec_cb = function(obj, value)
            obj:set { y = value }
        end
    }
    
    -- Auto-destroy after duration
    notification.timer = lvgl.Timer.create(function()
        notification:delete()
    end, duration, 1)
    
    return notification
end


local loadingDone = false
local loadingPopUpOverlay
local loadingPopUpOverlayParent
local function loadingPopUpRemove()
    if loadingPopUpOverlay then loadingPopUpOverlay:delete() end
    if loadingPopUpOverlayParent then loadingPopUpOverlayParent:delete() end
    loadingPopUpOverlay = nil
    loadingPopUpOverlayParent = nil
end
function utils.loadingPopUpAdd(parent,loadingText,loadingFunction)
    if not loadingPopUpOverlay then
        if not parent then   
            parent = lvgl.Object({
            w = W,
            h = H,
            align = lvgl.ALIGN.CENTER,
        })
        loadingPopUpOverlayParent = parent
        end
        loadingPopUpOverlay = parent:Object {
            w = W, h = H, x = 0, y = 0,
            bg_opa = 200, border_width = 0, pad_all = 0,
        }
        loadingPopUpOverlay:clear_flag(lvgl.FLAG.SCROLLABLE)
        local box = loadingPopUpOverlay:Object {
            w = 220, h = 100, align = lvgl.ALIGN.CENTER,
            border_width = 1, pad_all = 10,
            flex = { flex_direction = "column", flex_wrap = "nowrap" },
        }
        box:clear_flag(lvgl.FLAG.SCROLLABLE)
        _gridnav_add(box, GRIDNAV_ROLLOVER)
        local popup_group = lvgl.group.get_default()
        popup_group:add_obj(box)
        box:Label { text = "Loading " .. loadingText .. "...", w = lvgl.PCT(100), h = 24 }
        
        loadingDone = false
        local loadingTimer = lvgl.Timer {
            period = 1,
            cb =  function(t)
                if not loadingDone then
                    loadingDone = loadingFunction()
                    if loadingDone then
                        loadingPopUpRemove()
                        t:delete()
                    end
                end
            end
        }
    end

end



return utils