local weenet = require "weenet"
local socket = require "socket"

local function client(name, addr)
    local fd, err = socket.connect(addr)
    if err then
        error("fail to connect: " .. err)
    end
    while true do
        local ok, err = socket.write(fd, "0123456789")
        if not ok then
            error("fail to write: " .. err)
        end
        local str, err = socket.read(fd)
        if err then
            error("fail to read: " .. err)
        end
        print(name, "echo back: ", str)
        weenet.sleep(300)
    end
end

weenet.sleep(500)   -- let server start first
weenet.spawn(client, "client tcp", "tcp://*:4444")
weenet.spawn(client, "client tcp4", "tcp4://*:4444")
weenet.spawn(client, "client tcp6", "tcp6://*:4444")
