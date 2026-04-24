-- Minimal app navigation helper
-- Uses the same back-navigation pattern as the notes app (which works)

local nav = {}

-- Navigate back to the launcher
function nav.goHome(root_to_delete)
    if root_to_delete then
        root_to_delete:delete()
    end
    local launcher = require("launcher")
    launcher.create()
end

-- Add a back button to a parent container
-- root_obj: the root lvgl object to delete when going back
function nav.backButton(parent, root_obj)
    local btn = parent:Button {
        w = 50,
        h = 22,
        align = lvgl.ALIGN.RIGHT_MID,
    }
    btn:Label { text = "Back", align = lvgl.ALIGN.CENTER }
    btn:onClicked(function()
        nav.goHome(root_obj)
    end)
    return btn
end

return nav
