local c = require "weenet.c"
local self = c.self()

local log = {
}

function log.printf(...)
    print(string.format(...))
end

log.errorf = log.printf

local PKIND_NOTIFY          = 0
local PKIND_REQUEST         = 1
local PKIND_RESPONSE        = 2
local PKIND_BROADCAST       = 3

local KIND_NAMES = {
    [0]     = "notify",
    [1]     = "request",
    [2]     = "response",
    [3]     = "broadcast",
}

local KIND_INTEGERS = {
    notify      = 0,
    request     = 1,
    response    = 2,
    broadcast   = 3,
}

local weenet = { }

local function pcall(f, ...)
    return xpcall(f, debug.traceback, ...)
end

local coroutine_pool = {}

-- coroutine indexed table, values:
--   nil, detached
--   false, result is not ready
--   table, result is ok
--   thread, in join
local coroutine_results = {}

local function coroutine_main(fn, ...)
    local self = coroutine.running()
    local args = {...}
    while true do
        local results = {fn(table.unpack(args))}
        local joiner = coroutine_results[self]
        assert(type(joiner) ~= "table")
        if joiner == false then
            coroutine_results[self] = results
            weenet.suspend("WAIT JOIN")
        elseif type(joiner) == "thread" then
            coroutine_results[self] = results
            weenet.wakeup(joiner)
            weenet.suspend("WAIT RESUME")
        end
        fn = nil
        coroutine_pool[self] = true
        fn = coroutine.yield "ZOMBIE"
        args = {coroutine.yield()}
    end
end

local function new_coroutine(fn)
    local co = next(coroutine_pool)
    if co == nil then
        return coroutine.create(function(...)
            coroutine_main(fn, ...)
        end)
    else
        coroutine_pool[co] = nil
        coroutine.resume(co, fn)
    end
    return co
end

-- lock {

local lock_queue = {}

function weenet.lock(obj)
    local self = coroutine.running()
    local queue = lock_queue[obj]
    if queue == nil then
        queue = {}
        lock_queue[obj] = queue
    end
    table.insert(queue, self)
    if #queue ~= 1 then
        weenet.suspend("WAIT LOCKER")
    end
    assert(self == queue[1], "erroneous lock algorithm")
end

function weenet.unlock(obj)
    local self = coroutine.running()
    local queue = lock_queue[obj]
    assert(self == queue[1], "unlock non-owned locker")
    table.remove(queue, 1)
    if #queue ~= 0 then
        weenet.wakeup(queue[1])
    end
end

-- } lock

function weenet.self()
    return self
end

local spawn_coroutines = {}

function weenet.join(co)
    assert(type(co) == "thread")
    local result = coroutine_results[co]
    if result == nil then
        return false, "join detached thread"
    elseif type(result) == "thread" then
        return false, "another thread waiting result of target thread"
    end
    if result == false then
        local self = coroutine.running()
        assert(co ~= self, "join self")
        coroutine_results[co] = self
        weenet.suspend("WAIT CHILD")
        result = coroutine_results[co]
        if result == nil then
            return false, "detached during join"
        end
    end
    assert(type(result) == "table")
    weenet.wakeup(co)
    return true, table.unpack(result)
end

function weenet.task(fn, ...)
    local co = new_coroutine(fn)
    spawn_coroutines[co] = {...}
    coroutine_results[co] = false
    return co
end

function weenet.spawn(fn, ...)
    local co = new_coroutine(fn)
    spawn_coroutines[co] = {...}
    return co
end

function weenet.detach(co)
    local result = coroutine_results[co]
    coroutine_results[co] = nil
    if type(result) == "thread" then
        weenet.wakeup(result)
    elseif type(result) == "table" then
        weenet.wakeup(co)
    end
end

local weak_key_metatable = { __mode = "k" }
local coroutine_status = setmetatable({}, weak_key_metatable)
local suspend_coroutines = {} -- = setmetatable({}, { __mode = "k" })
local block_coroutines = {}

function weenet.yield()
    return weenet.sleep(0)
end

function weenet.sleep(time)
    local session = c.timeout(time)
    local result = coroutine.yield("SLEEP", session)
    suspend_coroutines[coroutine.running()] = nil
    return result
end

function weenet.suspend()
    local session = c.new_session()
    coroutine.yield("SUSPEND", session)
    suspend_coroutines[coroutine.running()] = nil
    block_coroutines[session] = nil
end

local wakeup_coroutines = {}

function weenet.wakeup(co)
    if suspend_coroutines[co] then
        wakeup_coroutines[co] = true
        return true
    end
end

function weenet.timeout(t, callback)
    local session = c.timeout(t)
    block_coroutines[session] = new_coroutine(callback)
end

function weenet.exit()
    c.exit()
    -- coroutine.yield "EXIT"
end

local service_name = "lua " .. SERVICE_NAME
local service_filename = SERVICE_FILENAME

function weenet.name()
    return service_name
end

function weenet.filename()
    return service_filename
end

local PROTOCOLS = {}

-- If session is nil or kind is "request", allocate new session.
function weenet.send(address, session, kind, protoname, ...)
    local proto = PROTOCOLS[protoname]
    return c.send(address, session, KIND_INTEGERS[kind], proto.code, proto[kind].pack(...))
end

function weenet.notify(address, protoname, ...)
    local proto = PROTOCOLS[protoname]
    return c.notify(address, proto.code, proto.notify.pack(...))
end

function weenet.request(address, protoname, ...)
    local proto = PROTOCOLS[protoname]
    local session = c.request(address, proto.code, proto.request.pack(...))
    local ok, msg = coroutine.yield("REQUEST", session)
    return p.response.unpack(msg)
end

local session_coroutine_address = {}
local session_coroutine_session = {}
local session_coroutine_protocol = {}

function weenet.response(...)
    local co = coroutine.running()
    local proto = session_coroutine_protocol[co]
    local source = session_coroutine_address[co]
    local session = session_coroutine_session[co]
    if session < 0 then
        error(string.format("protocol[%s]: duplicated response", proto.name))
    end
    session_coroutine_session[co] = -session
    return c.response(source, session, proto.code, proto.response.pack(...))
end

function weenet.protocol(proto)
    local code = proto.code
    local name = proto.name
    assert(type(code) == "number", "proto code must be a number")
    assert(type(name) == "string", "proto name must be a string")
    PROTOCOLS[code] = proto
    PROTOCOLS[name] = proto
end

function weenet.error(err)
    print(weenet.name() .. err)
end

local message_queue = {}
local message_coroutines = {}
local waiting_coroutines = {}

local function next_message(co)
    if #message_queue == 0 then
        table.insert(waiting_coroutines, co)
        weenet.suspend()
    end
    return table.remove(message_queue, 1)
end

local function handle_message(func, kind, proto, source, session, ...)
    local co = coroutine.running()
    session_coroutine_address[co] = source
    session_coroutine_session[co] = session
    session_coroutine_protocol[co] = proto
    local ok, errmsg = pcall(func, source, session,  ...)
    if not ok then
        weenet.error(string.format("failed to handle message: code[%d] name[%s] from source[%d]", proto.code, proto.name, source))
    end
    session_coroutine_address[co] = nil
    session_coroutine_session[co] = nil
    session_coroutine_protocol[co] = nil
end

local function do_work()
    local co = coroutine.running()
    while true do
        local msg = assert(next_message(co))
        handle_message(msg.func, msg.kind, msg.proto, msg.source, msg.session, table.unpack(msg))
    end
end

local function coroutine_dead(co)
    if message_coroutines[co] then
        message_coroutines[co] = nil
        local co = weenet.spawn(do_work)
        message_coroutines[co] = true
    end
end

local function trace(co, ok, status, session, ...)
    if not ok then
        local proto = session_coroutine_protocol[co]
        local source = session_coroutine_address[co]
        local session = session_coroutine_session[co]
        if session and session ~= 0 then
            c.send(source, session, PKIND_RESPONSE, PCODE_ERROR, "runtime error")
        end
        local traceback = debug.traceback(co, status)
        weenet.error(traceback)
        session_coroutine_address[co] = nil
        session_coroutine_session[co] = nil
        session_coroutine_protocol[co] = nil

        coroutine_dead(co)
        return
    end
    if status == "REQUEST" then
        block_coroutines[session] = co
    elseif status == "SLEEP" or status == "SUSPEND" then
        block_coroutines[session] = co
        suspend_coroutines[co] = session
    elseif status == "ZOMBIE" then
    else
        weenet.error(string.format("unknown yield status: %s\n%s", status, debug.traceback(co)))
        return
    end
    coroutine_status[co] = status
end

local function schedule_spawn()
    local co, args = next(spawn_coroutines)
    while co do
        spawn_coroutines[co] = nil
        trace(co, coroutine.resume(co, table.unpack(args)))
        co, args = next(spawn_coroutines)
    end
end

local function schedule_wakeup()
    local co = next(wakeup_coroutines)
    while co do
        wakeup_coroutines[co] = nil
        local session = suspend_coroutines[co]
        if session then
            block_coroutines[session] = "BREAK"
            trace(co, coroutine.resume(co, "BREAK"))
        end
        co = next(wakeup_coroutines)
    end
end

local function schedule()
    while next(spawn_coroutines) or next(wakeup_coroutines) do
        schedule_spawn()
        schedule_wakeup()
    end
end

local function unknown_response(source, session, code)
    weenet.error(string.format("unknown response: from[%d] session[%d] code[%d]", source, session, code))
end

local dispatch_mode

function weenet.mode(mode)
    dispatch_mode = mode
    if mode == "queue" and next(message_coroutines) == nil then
        local co = weenet.spawn(do_work)
        message_coroutines[co] = true
    end
end

function weenet.abort(err)
    error(weenet.name() .. err)
end

local function dispatch_message(source, session, kind, code, data, meta)
    kind = assert(KIND_NAMES[kind], "invalid message kind")
    if kind == "response" then
        local co = block_coroutines[session]
        if co == nil then
            unknown_response(source, session, code)
        elseif co == "BREAK" then
            block_coroutines[session] = nil
        else
            block_coroutines[session] = nil
            trace(co, coroutine.resume(co, true, data, meta))
        end
    else
        local proto = assert(PROTOCOLS[code], "unknown message")
        local msg = assert(proto[kind], "unsupported message kind")
        local func = assert(msg.func, "no handler function registered")

        if dispatch_mode == "queue" then
            table.insert(message_queue, {
                func = func,
                kind = kind,
                proto = proto,
                address = source,
                session = session,
                msg.unpack(data, meta)
            })
            if #waiting_coroutines ~= 0 then
                local co = table.remove(waiting_coroutines)
                weenet.wakeup(co)
            end
        else
            weenet.spawn(handle_message, func, kind, proto, source, session, msg.unpack(data, meta))
        end
    end
    schedule()
end

function weenet.start(func, ...)
    local session = c.bootstrap()
    block_coroutines[session] = new_coroutine(function(...)
        local ok, err = pcall(func, ...)
        if not ok then
            log.errorf("service[%s] failed to start: %s", weenet.name(), err)
            weenet.exit()
        end
    end)
end

c.callback(dispatch_message)

return weenet
