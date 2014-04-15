local weenet = require "weenet"
local socket = require "socket"

local reader, writer, err = socket.pipe()
if err then
    print(err)
end

local function do_read()
    print("start reading")
    while true do
        local str, err = socket.read(reader, 10)
        if err then
            print("read error:", err)
            break
        end
        print(string.format("recv: %s", str))
    end
    socket.close(reader)
    print("done read")
end

local function do_write()
    print("start writing")
    local i = 0
    while true do
        local _, err = socket.write(writer, "0123456789")
        i = i+1
        if err then
            print("write error:", err)
            break
        end
        if i > 1000 then
            break
        end
        weenet.yield()
    end
    socket.close(writer)
    print("Done: write")
end

local function main()
    print("in main")
    local reader = weenet.task(do_read)
    local writer = weenet.task(do_write)
    weenet.join(reader)
    weenet.join(writer)
    print("Done: main")
    weenet.exit()
end

print("execing ...")

weenet.start(function()
    print("starting ...");
    weenet.spawn(main)
end)
