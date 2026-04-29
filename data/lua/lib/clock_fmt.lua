local M = {}

function M.get()
    return _clock_fmt_get()
end

function M.set(v)
    return _clock_fmt_set(v)
end

return M
