-- cct_panel.lua — CCT Analysis panel for darktable (PRD-CCT-001 §9).
-- Queues pixel-level white-balance analysis against the RawImport service and
-- applies the computed D50-adapted hue/chroma to the Color Calibration module.
--
-- Install: copy into darktable's lua config dir (e.g.
--   ~/.config/darktable/lua/) and require it from ~/.config/darktable/luarc:
--     require "cct_panel"
--
-- Requirements:
--   RAWIMPORT_API_URL / API_TOKEN env vars (same as betterembeds.lua)
--   IMPORTANT (verified upstream, w1ne/darktable-mcp): dt.gui.action on
--   iop/* parameters only works while the target image is open in the
--   darkroom view. The Apply button checks this and prints guidance instead
--   of silently doing nothing.

local function api_base_url()
    if RAWIMPORT_API_URL and RAWIMPORT_API_URL ~= "" then return RAWIMPORT_API_URL end
    return os.getenv("RAWIMPORT_API_URL") or ""
end

local function api_token()
    if API_TOKEN and API_TOKEN ~= "" then return API_TOKEN end
    return os.getenv("API_TOKEN") or ""
end

local function curl_base_args()
    local tok = api_token()
    return (tok ~= "") and (' -H "Authorization: Bearer ' .. tok .. '"') or ""
end

-- Resolve the RawImport sequence name for an image via by-source lookup;
-- falls back to the filename stem when the service has no record.
local function resolve_sequence(image)
    local base = api_base_url()
    if base ~= "" and image.path then
        local sp = image.path .. "/" .. image.filename
        local esc = sp:gsub("([^%w%-%._~/])", function(c)
            return string.format("%%%02X", string.byte(c))
        end)
        local cmd = string.format('curl -s%s "%s/api/v1/imports/by-source?path=%s"',
            curl_base_args(), base, esc)
        local pipe = io.popen(cmd)
        if pipe then
            local out = pipe:read("*a")
            pipe:close()
            local seq = out:match('"sequence"%s*:%s*"([^"]+)"')
            if seq then return seq end
        end
    end
    return image.filename:gsub("%..*$", "")
end

local function request_cct_analysis(seq, algorithm, area)
    local base = api_base_url()
    if base == "" then return false, "RAWIMPORT_API_URL not set" end
    local body = string.format('{"sequence":"%s","algorithm":"%s","area":"%s"}',
        seq, algorithm, area)
    local cmd = string.format(
        'curl -s -X POST%s -H "Content-Type: application/json" -d \'%s\' "%s/api/v1/cct/analyze"',
        curl_base_args(), body, base)
    local pipe = io.popen(cmd)
    if not pipe then return false, "curl failed" end
    local out = pipe:read("*a")
    pipe:close()
    if out:match('"job_id"') then return true else return false, out end
end

local function fetch_cct(seq, algorithm)
    local base = api_base_url()
    if base == "" then return nil, nil, nil, nil, nil, "no-api-url" end
    local cmd = string.format(
        'curl -s%s "%s/api/v1/cct/result?sequence=%s&algorithm=%s"',
        curl_base_args(), base, seq, algorithm)
    local pipe = io.popen(cmd)
    if not pipe then return nil, nil, nil, nil, nil, "curl failed" end
    local out = pipe:read("*a")
    pipe:close()

    local status = out:match('"status"%s*:%s*"([^"]+)"')
    if status ~= "completed" then return nil, nil, nil, nil, nil, status end

    local cct  = tonumber(out:match('"cct_kelvin"%s*:%s*([%d%.]+)'))
    local tint = tonumber(out:match('"tint"%s*:%s*([%d%.%-]+)'))
    local hue  = tonumber(out:match('"hue"%s*:%s*([%d%.%-]+)'))
    local chroma = tonumber(out:match('"chroma"%s*:%s*([%d%.%-]+)'))
    local conf = tonumber(out:match('"confidence"%s*:%s*([%d%.]+)'))
    return cct, tint, hue, chroma, conf, status
end

-- Preflight (see header note): applying parameters requires the target image
-- to be open in the darkroom view; otherwise dt.gui.action is a silent no-op.
local function darkroom_ready_for(image)
    if dt.gui.current_view() ~= dt.gui.views.darkroom then
        return false, "open the image in the darkroom first"
    end
    local dr = dt.gui.get_darkroom_image()
    if not dr or dr.id ~= image.id then
        return false, "this image is not the one open in the darkroom"
    end
    return true
end

local function apply_cct_to_colorcal(image, cct, tint, hue, chroma, confidence)
    if not cct then return false end
    local ok, why = darkroom_ready_for(image)
    if not ok then
        dt.print("CCT: cannot apply - " .. why)
        return false
    end
    if confidence and confidence < 0.5 then
        dt.print(string.format("WARN: low confidence (%.0f%%); applying anyway",
                               confidence * 100))
    end
    -- Switch Color Calibration illuminant to custom, then push hue/chroma.
    pcall(function()
        dt.gui.action("iop/channelmixerrgb/illuminant", 0, "selection", "item:custom", 1.0)
    end)
    if hue then
        pcall(function() dt.gui.action("iop/channelmixerrgb/hue", 0, "", "set:" .. tostring(hue), 1.0) end)
    end
    if chroma then
        pcall(function() dt.gui.action("iop/channelmixerrgb/chroma", 0, "", "set:" .. tostring(chroma), 1.0) end)
    end
    dt.print(string.format("Applied CCT %.0fK (tint %.2f, conf %.0f%%)",
                           cct, tint or 0, (confidence or 0) * 100))
    return true
end

-- ---------------------------------------------------------------- panel --
local ALGOS = {
    "rawpy_grayworld",        -- channel mean (Minkowski p=1)
    "rawpy_shadesofgrey",     -- Minkowski norm p=6 (Finlayson & Trezzi 2004)
    "rawpy_whitepatch",       -- per-channel 99th percentile
    "rawpy_maxrgb",           -- per-channel 95th percentile
    "rawpy_generalgrayworld", -- Gaussian center-weighted mean
    "rawpy_bayesian",         -- gray-world + D65 prior blend
}
local AREAS = { "full", "center20", "center10" }

local cct_algo_combo = dt.new_widget("combobox"){
    label = "Algorithm", value = 1,
    table.unpack(ALGOS),
}
local cct_area_combo = dt.new_widget("combobox"){
    label = "Sample area", value = 1,
    table.unpack(AREAS),
}
local cct_status_lbl = dt.new_widget("label"){ label = "Status: ready", selectable = true }
local cct_result_lbl = dt.new_widget("label"){ label = "Result: --", selectable = true }

local analyze_btn = dt.new_widget("button"){
    label = "Analyze white balance",
    clicked_callback = function()
        local images = dt.gui.selection()
        if not images or #images == 0 then
            dt.print("CCT: no images selected")
            return
        end
        local algo = ALGOS[cct_algo_combo.selected]
        local area = AREAS[cct_area_combo.selected]
        for _, img in ipairs(images) do
            local seq = resolve_sequence(img)
            local ok, err = request_cct_analysis(seq, algo, area)
            if ok then
                cct_status_lbl.label = "Status: queued " .. seq
            else
                cct_status_lbl.label = "Status: failed (" .. tostring(err) .. ")"
            end
        end
    end,
}

local apply_btn = dt.new_widget("button"){
    label = "Apply to Color Calibration",
    clicked_callback = function()
        local images = dt.gui.selection()
        if not images or #images == 0 then return end
        local img = images[1]  -- apply operates on the darkroom image only
        local seq = resolve_sequence(img)
        local algo = ALGOS[cct_algo_combo.selected]
        local cct, tint, hue, chroma, conf, status = fetch_cct(seq, algo)
        if cct then
            cct_result_lbl.label = string.format(
                "Result: %.0fK | tint %.2f | conf %.0f%%",
                cct, tint or 0, (conf or 0) * 100)
            apply_cct_to_colorcal(img, cct, tint, hue, chroma, conf)
        elseif status then
            cct_result_lbl.label = "Result: " .. tostring(status)
        else
            cct_result_lbl.label = "Result: fetch failed"
        end
    end,
}

local cct_panel = dt.new_widget("box"){
    orientation = "vertical",
    dt.new_widget("label"){ label = "CCT Analysis", selectable = true },
    dt.new_widget("label"){ label = "Pixel-level illuminant estimation", selectable = true },
    dt.new_widget("label"){ label = " " },
    dt.new_widget("label"){ label = "Algorithm:", selectable = true },
    cct_algo_combo,
    dt.new_widget("label"){ label = "Sample area:", selectable = true },
    cct_area_combo,
    dt.new_widget("label"){ label = " " },
    analyze_btn,
    cct_status_lbl,
    dt.new_widget("label"){ label = " " },
    apply_btn,
    cct_result_lbl,
}

pcall(function()
    dt.register_lib(
        "cct_analysis_panel",
        "CCT Analysis",
        true,
        true,
        { [dt.gui.views.lighttable] = { "DT_UI_CONTAINER_PANEL_RIGHT_CENTER", 100 } },
        cct_panel
    )
end)
