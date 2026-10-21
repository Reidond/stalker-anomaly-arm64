-- jit_lab workloads. Loaded by lab.cpp with one argument: the `lab` table of C helpers
--   lab.now()          monotonic seconds
--   lab.add(a, b)      trivial C function (Lua -> C call cost)
--   lab.call_n(f, n)   C calls f(i) n times through lua_pcall (engine -> script callbacks)
--   lab.nest(f, n)     Lua -> C -> Lua -> C chains
--
-- Every benchmark takes a scale factor and returns a checksum. The checksum of the JIT run must
-- match the interpreter run (within a tiny tolerance: the arm64 JIT fuses multiply-add).
local lab = ...

local sqrt, sin, cos, floor = math.sqrt, math.sin, math.cos, math.floor
local format, byte, char, sub = string.format, string.byte, string.char, string.sub
local insert, remove, concat = table.insert, table.remove, table.concat

-- Deterministic PRNG whose intermediate values stay exact in a double.
local function lcg(seed)
  local s = seed
  return function()
    s = (s * 1664525 + 1013904223) % 4294967296
    return s
  end
end

local B = {}  -- name -> { group = , scale = , fn = , float = }

local function bench(group, name, scale, float, fn)
  B[#B + 1] = { group = group, name = name, scale = scale, float = float, fn = fn }
end

------------------------------------------------------------------------------------------------
-- Numeric
------------------------------------------------------------------------------------------------

bench("numeric", "nbody", 1.0, true, function(scale)
  local PI = math.pi
  local SOLAR_MASS = 4 * PI * PI
  local DPY = 365.24
  local bodies = {
    { x = 0, y = 0, z = 0, vx = 0, vy = 0, vz = 0, mass = SOLAR_MASS },
    { x = 4.84143144246472090e+00, y = -1.16032004402742839e+00, z = -1.03622044471123109e-01,
      vx = 1.66007664274403694e-03 * DPY, vy = 7.69901118419740425e-03 * DPY,
      vz = -6.90460016972063023e-05 * DPY, mass = 9.54791938424326609e-04 * SOLAR_MASS },
    { x = 8.34336671824457987e+00, y = 4.12479856412430479e+00, z = -4.03523417114321381e-01,
      vx = -2.76742510726862411e-03 * DPY, vy = 4.99852801234917238e-03 * DPY,
      vz = 2.30417297573763929e-05 * DPY, mass = 2.85885980666130812e-04 * SOLAR_MASS },
    { x = 1.28943695621391310e+01, y = -1.51111514016986312e+01, z = -2.23307578892655734e-01,
      vx = 2.96460137564761618e-03 * DPY, vy = 2.37847173959480950e-03 * DPY,
      vz = -2.96589568540237556e-05 * DPY, mass = 4.36624404335156298e-05 * SOLAR_MASS },
    { x = 1.53796971148509165e+01, y = -2.59193146099879641e+01, z = 1.79258772950371181e-01,
      vx = 2.68067772490389322e-03 * DPY, vy = 1.62824170038242295e-03 * DPY,
      vz = -9.51592254519715870e-05 * DPY, mass = 5.15138902046611451e-05 * SOLAR_MASS },
  }
  local nbody = #bodies
  local px, py, pz = 0, 0, 0
  for i = 1, nbody do
    local b = bodies[i]
    px = px + b.vx * b.mass; py = py + b.vy * b.mass; pz = pz + b.vz * b.mass
  end
  bodies[1].vx = -px / SOLAR_MASS; bodies[1].vy = -py / SOLAR_MASS; bodies[1].vz = -pz / SOLAR_MASS

  local function advance(dt)
    for i = 1, nbody do
      local bi = bodies[i]
      local bix, biy, biz, bimass = bi.x, bi.y, bi.z, bi.mass
      local bivx, bivy, bivz = bi.vx, bi.vy, bi.vz
      for j = i + 1, nbody do
        local bj = bodies[j]
        local dx, dy, dz = bix - bj.x, biy - bj.y, biz - bj.z
        local d2 = dx * dx + dy * dy + dz * dz
        local mag = sqrt(d2); mag = dt / (mag * d2)
        local bm = bj.mass * mag
        bivx = bivx - dx * bm; bivy = bivy - dy * bm; bivz = bivz - dz * bm
        bm = bimass * mag
        bj.vx = bj.vx + dx * bm; bj.vy = bj.vy + dy * bm; bj.vz = bj.vz + dz * bm
      end
      bi.vx = bivx; bi.vy = bivy; bi.vz = bivz
      bi.x = bix + dt * bivx; bi.y = biy + dt * bivy; bi.z = biz + dt * bivz
    end
  end
  local function energy()
    local e = 0
    for i = 1, nbody do
      local bi = bodies[i]
      local vx, vy, vz, bim = bi.vx, bi.vy, bi.vz, bi.mass
      e = e + 0.5 * bim * (vx * vx + vy * vy + vz * vz)
      for j = i + 1, nbody do
        local bj = bodies[j]
        local dx, dy, dz = bi.x - bj.x, bi.y - bj.y, bi.z - bj.z
        e = e - (bim * bj.mass) / sqrt(dx * dx + dy * dy + dz * dz)
      end
    end
    return e
  end
  for _ = 1, floor(200000 * scale) do advance(0.01) end
  return energy()
end)

bench("numeric", "mandelbrot", 1.0, true, function(scale)
  local n = floor(300 * scale ^ 0.5)
  local inside = 0
  for py = 0, n - 1 do
    local ci = 2.0 * py / n - 1.0
    for px = 0, n - 1 do
      local cr = 2.0 * px / n - 1.5
      local zr, zi, k = 0.0, 0.0, 0
      while k < 50 and zr * zr + zi * zi < 4.0 do
        zr, zi = zr * zr - zi * zi + cr, 2.0 * zr * zi + ci
        k = k + 1
      end
      if k == 50 then inside = inside + 1 end
    end
  end
  return inside
end)

bench("numeric", "sieve_loops", 1.0, false, function(scale)
  local n = floor(3000000 * scale)
  local flags = {}
  for i = 2, n do flags[i] = true end
  for i = 2, floor(sqrt(n)) do
    if flags[i] then
      for j = i * i, n, i do flags[j] = false end
    end
  end
  local count = 0
  for i = 2, n do if flags[i] then count = count + 1 end end
  local acc = 0
  for i = 1, floor(5000000 * scale) do acc = (acc + i * 7 + (i % 13)) % 1000003 end
  return count * 1000003 + acc
end)

bench("numeric", "fib_recursion", 1.0, false, function(scale)
  local function fib(n) if n < 2 then return n end return fib(n - 1) + fib(n - 2) end
  return fib(floor(27 + math.log(scale) / math.log(1.618)))
end)

------------------------------------------------------------------------------------------------
-- Tables and strings
------------------------------------------------------------------------------------------------

bench("table/string", "table_sort_stack", 1.0, false, function(scale)
  local n = floor(120000 * scale)
  local rnd = lcg(12345)
  local t = {}
  for i = 1, n do t[i] = rnd() % 100000 end
  table.sort(t)
  local rnd2 = lcg(777)
  local objs = {}
  for i = 1, floor(n / 4) do objs[i] = { key = rnd2() % 5000, id = i } end
  table.sort(objs, function(a, b) if a.key ~= b.key then return a.key < b.key end return a.id < b.id end)
  local stack, acc = {}, 0
  for i = 1, floor(600000 * scale) do
    insert(stack, i)
    if i % 3 == 0 then acc = acc + remove(stack) end
  end
  return t[1] + t[n] + objs[1].id + objs[#objs].id + acc + #stack
end)

bench("table/string", "hash_strings", 1.0, false, function(scale)
  local n = floor(80000 * scale)
  local map, order = {}, {}
  for i = 1, n do
    local k = "item_" .. (i % 9973) .. "_" .. (i % 7)
    map[k] = (map[k] or 0) + i
    if i % 5 == 0 then order[#order + 1] = k end
  end
  local acc = 0
  for rep = 1, 6 do
    for k, v in pairs(map) do acc = (acc + v + #k) % 1000000007 end
  end
  for i = 1, #order do map[order[i]] = nil end
  local left = 0
  for _ in pairs(map) do left = left + 1 end
  return acc + left
end)

bench("table/string", "string_ops", 1.0, false, function(scale)
  local n = floor(60000 * scale)
  local parts, acc = {}, 0
  for i = 1, n do
    local s = format("%d:%s:%.2f", i, ("ab"):rep(i % 5), i / 7)
    parts[#parts + 1] = s
    acc = acc + #s + byte(s, 1) + byte(s, -1)
    if i % 10 == 0 then
      local u = s:upper():gsub("%d", "#")
      acc = acc + #u
    end
    if s:find("ab", 1, true) then acc = acc + 1 end
    acc = acc + #sub(s, 2, 5) + (tonumber(s:match("^(%d+)")) or 0) % 7
  end
  local joined = concat(parts, ",")
  local words = 0
  for _ in joined:gmatch("[^,]+") do words = words + 1 end
  return acc + #joined + words
end)

------------------------------------------------------------------------------------------------
-- Anomaly-like: many small calls through closures and metatables
------------------------------------------------------------------------------------------------

bench("anomaly-like", "oop_metatables", 1.0, true, function(scale)
  local Base = {}
  Base.__index = Base
  function Base.new(x, y) return setmetatable({ x = x, y = y, hp = 100 }, Base) end
  function Base:update(dt) self.x = self.x + dt; return self.x end
  function Base:hurt(d) self.hp = self.hp - d; if self.hp < 0 then self.hp = 100 end end
  local Npc = setmetatable({}, { __index = Base })
  Npc.__index = Npc
  function Npc.new(x, y) local o = Base.new(x, y); o.state = "idle"; return setmetatable(o, Npc) end
  function Npc:update(dt)
    Base.update(self, dt)
    if self.hp < 30 then self.state = "flee" else self.state = "idle" end
    return self.y
  end
  local Stalker = setmetatable({}, { __index = Npc })
  Stalker.__index = Stalker
  function Stalker.new(x, y) local o = Npc.new(x, y); o.ammo = 30; return setmetatable(o, Stalker) end
  function Stalker:shoot() self.ammo = self.ammo - 1; if self.ammo <= 0 then self.ammo = 30 end return self.ammo end
  -- Defaults through an __index function, as script classes do.
  local defaults = setmetatable({}, { __index = function(_, k) return #k end })
  local objs = {}
  for i = 1, 400 do objs[i] = (i % 3 == 0) and Stalker.new(i, i * 2) or Npc.new(i, i * 2) end
  local acc = 0
  for frame = 1, floor(400 * scale) do
    for i = 1, #objs do
      local o = objs[i]
      acc = acc + o:update(0.016)
      o:hurt(frame % 7)
      if o.shoot then acc = acc + o:shoot() end
      acc = acc + defaults.somekey
    end
  end
  return acc
end)

bench("anomaly-like", "closures_varargs", 1.0, false, function(scale)
  local function counter(step)
    local n = 0
    return function(...)
      n = n + step + select("#", ...)
      return n
    end
  end
  local fns = {}
  for i = 1, 200 do fns[i] = counter(i % 5 + 1) end
  local function sum(...)
    local s = 0
    for i = 1, select("#", ...) do s = s + (select(i, ...)) end
    return s
  end
  local acc = 0
  for rep = 1, floor(2500 * scale) do
    for i = 1, #fns do
      acc = (acc + fns[i](rep, i, acc % 3)) % 1000003
    end
    local ok, v = pcall(sum, rep, 1, 2, 3)
    acc = acc + (ok and v or 0)
    local packed = { n = 3, rep, 2, 3 }
    acc = acc + select("#", unpack(packed, 1, packed.n))
  end
  return acc
end)

bench("anomaly-like", "event_bus", 1.0, false, function(scale)
  local callbacks = {}
  local function register(name, fn)
    local list = callbacks[name]
    if not list then list = {}; callbacks[name] = list end
    list[#list + 1] = fn
  end
  local function send(name, ...)
    local list = callbacks[name]
    if not list then return 0 end
    local n = 0
    for i = 1, #list do
      if list[i](...) then n = n + 1 end
    end
    return n
  end
  local total = 0
  local events = { "npc_on_update", "actor_on_update", "on_item_take", "npc_on_death", "monster_on_update" }
  for e = 1, #events do
    for h = 1, 12 do
      register(events[e], function(obj, dt)
        total = total + (obj.id % (h + 1)) + (dt > 0.5 and 1 or 0)
        return obj.id % 2 == 0
      end)
    end
  end
  local payload = { id = 0 }
  local acc = 0
  for i = 1, floor(60000 * scale) do
    payload.id = i
    acc = acc + send(events[i % #events + 1], payload, (i % 10) / 10)
  end
  return acc + total
end)

bench("anomaly-like", "ai_update", 1.0, true, function(scale)
  local rnd = lcg(4242)
  local npcs = {}
  for i = 1, 600 do
    npcs[i] = { x = rnd() % 1000, y = rnd() % 1000, z = 0, state = "idle", hp = 100, target = false, id = i }
  end
  local player = { x = 500, y = 500, z = 0 }
  local visible = {}
  local log_lines = 0
  local acc = 0
  for tick = 1, floor(300 * scale) do
    local n = 0
    for i = 1, #npcs do
      local npc = npcs[i]
      local dx, dy = npc.x - player.x, npc.y - player.y
      local dist = sqrt(dx * dx + dy * dy)
      local st = npc.state
      if st == "idle" then
        if dist < 120 then npc.state = "alert" end
      elseif st == "alert" then
        if dist < 60 then npc.state = "attack"; npc.target = player
        elseif dist > 200 then npc.state = "idle" end
      elseif st == "attack" then
        npc.hp = npc.hp - (rnd() % 3)
        if npc.hp <= 0 then npc.state = "dead" elseif dist > 120 then npc.state = "alert" end
      end
      if st ~= "dead" then
        npc.x = npc.x + (rnd() % 3 - 1) * 0.5
        npc.y = npc.y + (rnd() % 3 - 1) * 0.5
        if dist < 150 then n = n + 1; visible[n] = npc end
      elseif tick % 50 == 0 then
        npc.state = "idle"; npc.hp = 100
      end
      acc = acc + dist
    end
    for i = n + 1, #visible do visible[i] = nil end
    if tick % 25 == 0 then
      local line = format("tick %d visible %d", tick, n)
      log_lines = log_lines + #line
    end
    player.x = 500 + 80 * sin(tick * 0.05); player.y = 500 + 80 * cos(tick * 0.05)
  end
  return acc + log_lines
end)

bench("anomaly-like", "pcall_errors", 1.0, false, function(scale)
  local function thrower(i)
    if i % 3 == 0 then error("bad " .. i) end
    return i
  end
  local function nested(i) return thrower(i) + 1 end
  local acc, fails = 0, 0
  for i = 1, floor(150000 * scale) do
    local ok, v = pcall(nested, i)
    if ok then acc = (acc + v) % 1000003 else fails = fails + 1; acc = acc + #v % 7 end
  end
  -- Errors raised in a Lua callback that C called, unwinding through C and JIT frames.
  local function cb(i)
    if i % 5 == 0 then error("cb") end
    return i
  end
  for _ = 1, floor(5 * scale) do
    local ok = pcall(lab.call_n, cb, 20000)
    fails = fails + (ok and 0 or 1)
  end
  return acc + fails
end)

------------------------------------------------------------------------------------------------
-- Callbacks between C and Lua (the engine's script exports and callbacks)
------------------------------------------------------------------------------------------------

bench("c-interop", "lua_to_c_calls", 1.0, false, function(scale)
  local add = lab.add
  local acc = 0
  for i = 1, floor(2500000 * scale) do acc = add(acc, i % 11) % 1000003 end
  return acc
end)

bench("c-interop", "c_calls_lua", 1.0, false, function(scale)
  local acc = 0
  local function cb(i) acc = (acc + i * 3) % 1000003; return acc end
  local total = 0
  for rep = 1, floor(20 * scale) do total = total + lab.call_n(cb, 50000) end
  return acc + total % 1000003
end)

bench("c-interop", "nested_c_lua_c", 1.0, false, function(scale)
  local add = lab.add
  local function inner(i) return add(i, 1) end
  local acc = 0
  for rep = 1, floor(15 * scale) do acc = (acc + lab.nest(inner, 20000)) % 1000003 end
  return acc
end)

------------------------------------------------------------------------------------------------
-- Stability: mixed workloads plus constantly generated code, checked against the interpreter
------------------------------------------------------------------------------------------------

local function close_enough(a, b, float)
  if a == b then return true end
  if not float then return false end
  local scale = math.max(1, math.abs(a), math.abs(b))
  return math.abs(a - b) <= 1e-8 * scale
end

local trace_stats = { start = 0, stop = 0, abort = 0, flush = 0 }
if jit and jit.attach then
  jit.attach(function(what)
    if what == "start" then trace_stats.start = trace_stats.start + 1
    elseif what == "stop" then trace_stats.stop = trace_stats.stop + 1
    elseif what == "abort" then trace_stats.abort = trace_stats.abort + 1
    elseif what == "flush" then trace_stats.flush = trace_stats.flush + 1 end
  end, "trace")
end

local gen_ops = { "+", "-", "*" }
local function make_generated(seed)
  local rnd = lcg(seed)
  local a, b, c = rnd() % 9 + 1, rnd() % 9 + 1, rnd() % 9 + 1
  local src = format([[
    local n = ...
    local s, t = 0, {}
    for i = 1, n do
      local x = i %s %d
      if x %% %d == 0 then s = s + x %s %d else s = s - 1 end
      t[(i %% 16) + 1] = (t[(i %% 16) + 1] or 0) + s
    end
    return s + #t
  ]], gen_ops[a % 3 + 1], a, b, gen_ops[c % 3 + 1], c)
  return assert(load(src, "=gen" .. seed))
end

local M = { benches = B, trace_stats = trace_stats }

-- jit.on/off/flush raise "permanently disabled" in builds without a JIT compiler.
local function tolerant(f)
  return function(...) pcall(f, ...) end
end
local jit_on, jit_off, jit_flush = tolerant(jit.on), tolerant(jit.off), tolerant(jit.flush)

-- Runs `seconds` of mixed work. `report(line)` gets a progress line every ~5 s.
function M.stability(seconds, report, want_jit)
  local kernels = {}
  for _, b in ipairs(B) do
    kernels[#kernels + 1] = { b = b, scale = 0.02 }
  end
  -- Interpreter reference results.
  jit_off(); jit_flush()
  for _, k in ipairs(kernels) do k.ref = k.b.fn(k.scale) end
  if want_jit then jit_on() end
  jit_flush()

  local start, last_report, last_flush = lab.now(), lab.now(), lab.now()
  local iters, mismatches, errors, gens, flushes = 0, 0, 0, 0, 0
  local first_error
  local seed = 1
  while lab.now() - start < seconds do
    local k = kernels[iters % #kernels + 1]
    local ok, v = pcall(k.b.fn, k.scale)
    if not ok then
      errors = errors + 1; first_error = first_error or (k.b.name .. ": " .. tostring(v))
    elseif not close_enough(v, k.ref, k.b.float) then
      mismatches = mismatches + 1
      first_error = first_error or format("%s: got %s want %s", k.b.name, tostring(v), tostring(k.ref))
    end
    -- Fresh code every iteration: new functions, new traces, new machine code.
    seed = seed + 1
    local gen = make_generated(seed)
    local r1 = gen(300)
    jit_off(gen); local r2 = gen(300); if want_jit then jit_on(gen) end
    gens = gens + 1
    if r1 ~= r2 then
      mismatches = mismatches + 1
      first_error = first_error or ("generated function " .. seed .. " differs")
    end
    iters = iters + 1
    if iters % 4 == 0 then collectgarbage("step", 200) end
    local t = lab.now()
    if t - last_flush >= 12 then
      jit_flush(); flushes = flushes + 1; last_flush = t
    end
    if t - last_report >= 5 then
      last_report = t
      report(format("  t=%4.0fs iters=%d generated=%d traces=%d aborts=%d flushes=%d mism=%d err=%d lua_kb=%d",
        t - start, iters, gens, trace_stats.stop, trace_stats.abort, flushes, mismatches, errors,
        collectgarbage("count")))
    end
  end
  return { seconds = lab.now() - start, iters = iters, generated = gens, mismatches = mismatches,
           errors = errors, flushes = flushes, traces = trace_stats.stop, aborts = trace_stats.abort,
           first_error = first_error }
end

return M
