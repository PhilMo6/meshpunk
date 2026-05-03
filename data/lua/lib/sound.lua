local sound = {}

function sound.setVolume(vol)   return _sound_set_volume(vol)        end
function sound.getVolume()      return _sound_get_volume()           end
function sound.isMuted()        return _sound_get_muted()            end
function sound.mute()           return _sound_set_muted(true)        end
function sound.unmute()         return _sound_set_muted(false)       end
function sound.isPlaying()      return _sound_is_playing()           end

function sound.toggleMute()
    if _sound_get_muted() then _sound_set_muted(false)
    else                       _sound_set_muted(true) end
end

local function make_obj(handle)
    return {
        play   = function(self) _sound_play(handle)   end,
        stop   = function(self) _sound_stop(handle)   end,
        pause  = function(self) _sound_pause(handle)  end,
        delete = function(self) _sound_delete(handle) end,
    }
end

function sound.generateTone(freq, duration_ms)
    local h = _sound_generate_tone(freq, duration_ms or 200)
    if not h or h < 0 then return nil, "tone generation failed" end
    return make_obj(h)
end

function sound.loadFile(file_handle)
    local h = _sound_load_file(file_handle)
    if not h or h < 0 then return nil, "failed to load file" end
    return make_obj(h)
end

return sound
