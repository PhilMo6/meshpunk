-- WiFi fetch example application
local wifi = require("lib/wifi")
local utils = require("lib/utils")

-- Create main UI
-- local root = lvgl.Object()
-- root:set{w = lvgl.HOR_RES(), h = lvgl.VER_RES()}
-- root:clear_flag(lvgl.FLAG.SCROLLABLE)

local root = lvgl.Object {
  flex = {
      flex_direction = "row",
      flex_wrap = "wrap",
      justify_content = "center",
      align_items = "center",
      align_content = "center",
  },
  w = lvgl.PCT(100),
  h = lvgl.PCT(100),
  align = lvgl.ALIGN.CENTER
}

-- Header
local header = root:Object{bg_color = "#1E3A8A", w = lvgl.PCT(100), h = 48}

header:Label{
  text = "WiFi Fetch Example",
  text_color = "#FFFFFF",
  text_font = lvgl.BUILTIN_FONT.MONTSERRAT_16,
  align = lvgl.ALIGN.CENTER
}

-- WiFi status section
local status_section = root:Object{w = lvgl.PCT(100), h = 60, y = 48}

local status_label = status_section:Label{
  text = "WiFi disconnected",
  align = lvgl.ALIGN.CENTER
}

-- Connect button
local connect_btn = root:Button{w = lvgl.PCT(45), h = 40}

connect_btn:Label{text = "Connect WiFi", align = lvgl.ALIGN.CENTER}

-- Fetch button
local fetch_btn = root:Button{
  w = lvgl.PCT(45),
  h = 40,
}

fetch_btn:Label{text = "Fetch RSS", align = lvgl.ALIGN.CENTER}
fetch_btn:add_state(lvgl.STATE.DISABLED)

-- Results area
local results_container = root:Object{
  w = lvgl.PCT(100),
  h = lvgl.VER_RES() - 170,
  y = 170
}

local results_label = results_container:Label{
  text = "Results will appear here...",
  w = lvgl.PCT(100),
  h = lvgl.PCT(100)
}

-- Coroutines for async operations
local connect_co = nil
local fetch_co = nil

-- Timer for coroutine processing
local timer = nil

-- Connect WiFi button handler
connect_btn:onClicked(function()
  if wifi.isConnected() then
    -- Disconnect
    wifi.disconnect()
    status_label.text = "WiFi disconnected"
    connect_btn:Label().text = "Connect WiFi"
    fetch_btn:add_state(lvgl.STATE.DISABLED)
    results_label.text = "Results will appear here..."
    return
  end
  
  -- Show WiFi connection dialog
  local ssid = "wiwi" -- Replace with your WiFi network name
  local password = "snowcrash" -- Replace with your WiFi password
  
  -- Change button state
  connect_btn:add_state(lvgl.STATE.DISABLED)
  status_label.text = "Connecting to WiFi..."
  
  -- Create connection coroutine
  connect_co = wifi.connect(ssid, password)
  
  -- Start timer to process coroutine
  -- if timer then timer:delete() end
  
  timer = lvgl.Timer {
    period = 100,
    cb = function(t)
      
      if connect_co then
        local success, result = wifi.resume(connect_co)
        
        if not success then
          status_label.text = "Error: " .. tostring(result)
          connect_btn:clear_state(lvgl.STATE.DISABLED)
          connect_co = nil
          return
        end
        
        if result == true then
          -- Connected successfully
          status_label.text = "WiFi connected!"
          connect_btn:clear_state(lvgl.STATE.DISABLED)
          connect_btn:Label().text = "Disconnect WiFi"
          fetch_btn:clear_state(lvgl.STATE.DISABLED)
          connect_co = nil
        end
      elseif fetch_co then
        local success, result = wifi.resume(fetch_co)
        
        if not success or result then
          -- Fetch completed or error
          fetch_btn:clear_state(lvgl.STATE.DISABLED)
          
          if success and result.success then
            results_label.text = result.body:sub(1, 1000) .. "..."
          else
            results_label.text = "Fetch error: " ..
            (result.error or
            "Unknown error")
          end
          
          fetch_co = nil
        end
      else
        -- No active coroutines, stop timer
        timer:delete()
        timer = nil
      end
    end
  }
end)

-- Fetch button handler
fetch_btn:onClicked(function()
  if not wifi.isConnected() then
    utils.createNotification(root, "WiFi not connected", 2000)
    return
  end
  
  -- Change button state
  fetch_btn:add_state(lvgl.STATE.DISABLED)
  results_label.text = "Fetching data..."
  
  -- Create fetch coroutine
  fetch_co = wifi.fetch(
  "https://bsky.app/profile/did:plc:ibddtfbwxfpmcpeth7afrojs/rss")
  
  -- Start timer to process coroutine if not already running
  timer = lvgl.Timer {
    period = 100,
    cb = function(t)
      if fetch_co then
        local success, result = wifi.resume(fetch_co)
        
        if not success or result then
          -- Fetch completed or error
          fetch_btn:clear_state(lvgl.STATE.DISABLED)
          
          if success and result.success then
            results_label.text = result.body:sub(1, 1000) .. "..."
          else
            results_label.text = "Fetch error: " ..
            (result.error or
            "Unknown error")
          end
          
          fetch_co = nil
          timer:delete()
          timer = nil
        end
      else
        -- No active coroutines, stop timer
        timer:delete()
        timer = nil
      end
    end
  }
end)

return root
