local obs = obslua

-- ==========================================
-- Глобальные переменные
-- ==========================================
local source_name = ""
local timings_text = ""
local mpc_url = "http://localhost:13579/variables.html"
local poll_interval = 1000

local parsed_intervals = {}
local is_censoring = nil
local current_time = -1

-- ==========================================
-- Инициализация WinInet (для бесшумных запросов на Windows)
-- ==========================================
local ffi_ok, ffi = pcall(require, "ffi")
local is_windows = package.config:sub(1,1) == "\\"
local use_wininet = ffi_ok and is_windows

local wininet
if use_wininet then
    pcall(function()
        ffi.cdef[[
            typedef void* HINTERNET;
            typedef unsigned long DWORD;
            typedef int BOOL;
            typedef const char* LPCSTR;
            typedef void* LPVOID;

            HINTERNET InternetOpenA(LPCSTR lpszAgent, DWORD dwAccessType, LPCSTR lpszProxy, LPCSTR lpszProxyBypass, DWORD dwFlags);
            HINTERNET InternetOpenUrlA(HINTERNET hInternet, LPCSTR lpszUrl, LPCSTR lpszHeaders, DWORD dwHeadersLength, DWORD dwFlags, DWORD dwContext);
            BOOL InternetReadFile(HINTERNET hFile, LPVOID lpBuffer, DWORD dwNumberOfBytesToRead, DWORD* lpdwNumberOfBytesRead);
            BOOL InternetCloseHandle(HINTERNET hInternet);
        ]]
        wininet = ffi.load("wininet")
    end)
    if not wininet then use_wininet = false end
end

-- ==========================================
-- Вспомогательные функции
-- ==========================================

local function time_to_seconds(h, m, s)
    return (h or 0) * 3600 + (m or 0) * 60 + (s or 0)
end

local function parse_timings(text)
    parsed_intervals = {}
    if not text or text == "" then return end
    
    local pattern = "(%d+):(%d+):(%d+)%s*%-%s*(%d+):(%d+):(%d+)"
    
    for h1, m1, s1, h2, m2, s2 in string.gmatch(text, pattern) do
        local start_sec = time_to_seconds(tonumber(h1), tonumber(m1), tonumber(s1))
        local end_sec = time_to_seconds(tonumber(h2), tonumber(m2), tonumber(s2))
        
        if start_sec > end_sec then
            start_sec, end_sec = end_sec, start_sec
        end
        
        table.insert(parsed_intervals, {start = start_sec, ["end"] = end_sec})
    end
    obs.script_log(obs.LOG_INFO, "Цензор: Загружено " .. #parsed_intervals .. " таймингов.")
end

-- Парсинг HTML страницы MPC-BE
local function parse_html(html)
    if not html or html == "" then return -1 end
    
    local h, m, s = string.match(html, '<p%s+id="positionstring"[^>]*>(%d+):(%d+):(%d+)</p>')
    if h and m and s then
        return time_to_seconds(tonumber(h), tonumber(m), tonumber(s))
    end
    
    local ms = string.match(html, '<p%s+id="position"[^>]*>(%d+)</p>')
    if ms then
        return math.floor(tonumber(ms) / 1000)
    end
    
    return -1
end

-- HTTP запрос через WinInet (Windows, без консоли)
local function fetch_wininet()
    local INTERNET_OPEN_TYPE_PRECONFIG = 0
    local INTERNET_FLAG_RELOAD = 0x80000000
    local INTERNET_FLAG_NO_CACHE_WRITE = 0x04000000
    local flags = bit.bor(INTERNET_FLAG_RELOAD, INTERNET_FLAG_NO_CACHE_WRITE)
    
    local hInternet = wininet.InternetOpenA("OBS Censor", INTERNET_OPEN_TYPE_PRECONFIG, nil, nil, 0)
    if hInternet == nil then return -1 end
    
    local hUrl = wininet.InternetOpenUrlA(hInternet, mpc_url, nil, 0, flags, 0)
    if hUrl == nil then
        wininet.InternetCloseHandle(hInternet)
        return -1
    end
    
    local buffer_size = 65536
    local buffer = ffi.new("char[?]", buffer_size)
    local bytes_read = ffi.new("DWORD[1]")
    local chunks = {}
    
    while true do
        local ok = wininet.InternetReadFile(hUrl, buffer, buffer_size - 1, bytes_read)
        if ok == 0 or bytes_read[0] == 0 then break end
        table.insert(chunks, ffi.string(buffer, bytes_read[0]))
    end
    
    wininet.InternetCloseHandle(hUrl)
    wininet.InternetCloseHandle(hInternet)
    
    local html = table.concat(chunks)
    return parse_html(html)
end

-- HTTP запрос через io.popen (Linux/Mac или fallback на Windows)
local function fetch_popen()
    local stderr_redirect = is_windows and "2>nul" or "2>/dev/null"
    
    local cmd = string.format('curl -s -f -m 1 "%s" %s', mpc_url, stderr_redirect)
    local handle = io.popen(cmd)
    local result = nil
    
    if handle then
        result = handle:read("*a")
        handle:close()
    end
    
    if not result or result == "" then
        cmd = string.format('wget -qO- --timeout=1 "%s" %s', mpc_url, stderr_redirect)
        handle = io.popen(cmd)
        if handle then
            result = handle:read("*a")
            handle:close()
        end
    end
    
    return parse_html(result)
end

-- Общая функция запроса времени
local function fetch_mpc_time()
    if not mpc_url or mpc_url == "" then return -1 end
    
    if use_wininet and wininet then
        return fetch_wininet()
    else
        return fetch_popen()
    end
end

local function get_source_safe()
    if not source_name or source_name == "" then return nil end
    return obs.obs_get_source_by_name(source_name)
end

local function check_and_toggle()
    if current_time == -1 then return end
    
    local is_in_interval = false
    for _, interval in ipairs(parsed_intervals) do
        if current_time >= interval.start and current_time <= interval["end"] then
            is_in_interval = true
            break
        end
    end
    
    if is_censoring == is_in_interval then return end
    
    local source = get_source_safe()
    if source == nil then
        if source_name ~= "" then
            obs.script_log(obs.LOG_WARNING, string.format("Цензор: Источник '%s' не найден. Возможно, удален.", source_name))
        end
        return
    end
    
    local current_scene_source = obs.obs_frontend_get_current_scene()
    if current_scene_source == nil then
        obs.obs_source_release(source)
        return
    end
    
    local scene = obs.obs_scene_from_source(current_scene_source)
    if scene == nil then
        obs.obs_source_release(current_scene_source)
        obs.obs_source_release(source)
        return
    end
    
    local scene_item = obs.obs_scene_find_source_recursive(scene, source_name)
    
    if scene_item ~= nil then
        obs.obs_sceneitem_set_visible(scene_item, is_in_interval)
        is_censoring = is_in_interval
        
        local state = is_in_interval and "ВКЛЮЧЕН" or "ВЫКЛЮЧЕН"
        local h = math.floor(current_time / 3600)
        local m = math.floor((current_time % 3600) / 60)
        local s = math.floor(current_time % 60)
        obs.script_log(obs.LOG_INFO, string.format("Цензор: '%s' %s (%02d:%02d:%02d)", source_name, state, h, m, s))
    end
    
    obs.obs_source_release(current_scene_source)
    obs.obs_source_release(source)
end

local function tick()
    current_time = fetch_mpc_time()
    check_and_toggle()
end

local function update_source_list(property)
    obs.obs_property_list_clear(property)
    obs.obs_property_list_add_string(property, "-- Не выбрано --", "")
    
    local sources = obs.obs_enum_sources()
    if sources then
        for _, source in ipairs(sources) do
            local name = obs.obs_source_get_name(source)
            if name and name ~= "" then
                obs.obs_property_list_add_string(property, name, name)
            end
            obs.obs_source_release(source)
        end
    end
end

-- ==========================================
-- API функции OBS
-- ==========================================

function script_description()
    local method = use_wininet and "WinInet (без окон консоли)" or "io.popen (curl/wget)"
    return string.format([[
        <b>Автоматический цензор для OBS Studio</b><br>
        Включает выбранный источник в зависимости от таймингов и времени MPC-BE.<br><br>
        <b>Формат таймингов:</b><br>
        <i>00:03:45-00:03:49 - описание</i><br>
        <i>01:28:19-01:26:21 - (опечатки исправляются автоматически)</i><br><br>
        <b>HTTP метод:</b> %s
    ]], method)
end

function script_properties()
    local props = obs.obs_properties_create()
    
    local p_source = obs.obs_properties_add_list(props, "source_name", "Источник-заглушка:", obs.OBS_COMBO_TYPE_LIST, obs.OBS_COMBO_FORMAT_STRING)
    update_source_list(p_source)
    
    obs.obs_properties_add_button(props, "refresh_sources", "Обновить список источников", function()
        update_source_list(p_source)
        return true
    end)
    
    obs.obs_properties_add_text(props, "mpc_url", "URL MPC-BE (variables.html):", obs.OBS_TEXT_DEFAULT)
    obs.obs_properties_add_int(props, "poll_interval", "Интервал опроса (мс):", 500, 5000, 100)
    obs.obs_properties_add_text(props, "timings_text", "Тайминги:", obs.OBS_TEXT_MULTILINE)
    
    return props
end

function script_defaults(settings)
    obs.obs_data_set_default_string(settings, "source_name", "")
    obs.obs_data_set_default_string(settings, "mpc_url", "http://localhost:13579/variables.html")
    obs.obs_data_set_int(settings, "poll_interval", 1000)
    obs.obs_data_set_default_string(settings, "timings_text", "")
end

function script_update(settings)
    source_name = obs.obs_data_get_string(settings, "source_name")
    timings_text = obs.obs_data_get_string(settings, "timings_text")
    mpc_url = obs.obs_data_get_string(settings, "mpc_url")
    poll_interval = obs.obs_data_get_int(settings, "poll_interval")
    
    parse_timings(timings_text)
    is_censoring = nil
    
    obs.timer_remove(tick)
    if poll_interval > 0 then
        obs.timer_add(tick, poll_interval)
    end
end

function script_unload()
    obs.timer_remove(tick)
end