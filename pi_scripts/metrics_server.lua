local socket = require("socket")

local function read_file(path)
  local f = io.open(path, "r")
  if not f then return nil end
  local v = f:read("*n")
  f:close()
  return v
end

local function get_temp()
  local raw = read_file("/sys/class/thermal/thermal_zone0/temp")
  if raw then return raw / 1000.0 end
  return nil
end

local function shell(cmd)
  local f = io.popen(cmd)
  if not f then return nil end
  local out = f:read("*a")
  f:close()
  return out
end

local function get_cpu()
  -- crude 1-sample load avg based %
  local out = shell("LC_NUMERIC=C top -bn1 | grep '%Cpu' | awk '{print $2}'")
  return tonumber(out) or 0
end

local function get_ram_percent()
    local out = shell("free | awk '/Mem/{printf \"%.1f\", $3/$2*100}'")
    return tonumber(out) or 0
end

local function get_ram_total()
    local out = shell("free | awk '/Mem/{printf \"%.1f\", $2}'")
    return tonumber(out) or 0
end

local function get_ram_used()
  local out = shell("free | awk '/Mem/{printf \"%.1f\", $3}'")
  return tonumber(out) or 0
end

local function get_disk_percent()
    local out = shell("df / | awk 'NR==2{gsub(\"%\",\"\"); print $5}'")
    return tonumber(out) or 0
end

local function get_disk_total()
    local out = shell("df / | awk 'NR==2{print $2}'")
    return tonumber(out) or 0
end

local function get_disk_used()
    local out = shell("df / | awk 'NR==2{print $3}'")
    return tonumber(out) or 0
end

local function get_containers_running()
    local out = shell("docker ps -f status=running -q | wc -l")
    return tonumber(out) or 0
end

local function get_containers_stopped()
    local out = shell("docker ps -f status=exited -q | wc -l")
    return tonumber(out) or 0
end


local function json_metrics()
    local t = get_temp()
    return string.format(
        '{"temp":%.1f,"cpu":%.1f,"ram_percent":%.1f,"ram_total":%.1f,"ram_used":%.1f,"disk_percent":%.1f,"disk_total":%.1f,"disk_used":%.1f,"containers_running":%d,"containers_stopped":%d}',
        t or 0,
        get_cpu(),
        get_ram_percent(),
        get_ram_total(),
        get_ram_used(),
        get_disk_percent(),
        get_disk_total(),
        get_disk_used(),
        get_containers_running(),
        get_containers_stopped()
    )
end

local function get_local_ip()
  local s = socket.udp()
  s:setpeername("8.8.8.8", 80)
  local ip = s:getsockname()
  s:close()
  return ip
end

local server = assert(socket.bind(get_local_ip(), 9991))
print("listening on 9991")

while true do
  local client = server:accept()
  client:settimeout(2)
  local request, err = client:receive("*l")
  if request then
    local body = json_metrics()
    client:send("HTTP/1.1 200 OK\r\n")
    client:send("Content-Type: application/json\r\n")
    client:send("Content-Length: " .. #body .. "\r\n")
    client:send("Connection: close\r\n\r\n")
    client:send(body)
  end
  client:close()
end
