local weenet = require "weenet"
local socket = require "socket"

local function echo(fd)
    while true do
        local str, err = socket.read(fd)
        if err then
            error("fail to read: " .. err)
        end
        local ok, err = socket.write(fd, str)
        if err then
            error("fail to write: " .. err)
        end
    end
end

local function main()
    local listener, err = socket.listen("tcp4://*:4444")
    if err then
        error("fail to listen: " .. err)
    end
    while true do
        local fd, err = socket.accept(listener)
        if err then
            error("fail to accept: " .. err)
        end
        weenet.spawn(echo, fd)
    end
end

weenet.start(function()
    weenet.spawn(main)
end)
