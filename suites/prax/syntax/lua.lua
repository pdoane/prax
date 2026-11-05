-- An event emitter with one-shot listeners, as a module.
local Emitter = {}
Emitter.__index = Emitter

local MAX_LISTENERS <const> = 16

function Emitter.new()
  return setmetatable({ listeners = {} }, Emitter)
end

function Emitter:on(name, fn, once)
  local list = self.listeners[name] or {}
  if #list >= MAX_LISTENERS then
    error(string.format("too many listeners for %q", name), 2)
  end
  list[#list + 1] = { fn = fn, once = once == true }
  self.listeners[name] = list
  return self
end

function Emitter:once(name, fn)
  return self:on(name, fn, true)
end

function Emitter:emit(name, ...)
  local list = self.listeners[name]
  if not list then return 0 end
  local called = 0
  for i = #list, 1, -1 do
    local listener = list[i]
    listener.fn(...)
    called = called + 1
    if listener.once then table.remove(list, i) end
  end
  return called
end

--[[ A short demonstration,
     run when the file is the main chunk. ]]
if not pcall(debug.getlocal, 4, 1) then
  local e = Emitter.new()
  e:on("tick", function(n) print(("tick %d"):format(n)) end)
  e:once("tick", function() print("first tick only\n") end)
  for n = 1, 2 do e:emit("tick", n) end
  print(e:emit("missing") == 0, 0x1F, 3.5e2, nil)
end

return Emitter
