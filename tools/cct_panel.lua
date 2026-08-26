-- cct_panel.lua — CCT Analysis panel for darktable (PRD v2.3.0 §8).
-- Queries the synchronous native endpoint GET /api/v1/cct/analyze and applies
-- the returned white-balance gains to the Color Calibration module.
--
-- Install: copy into darktable's lua config dir and require from luarc:
--   require "cct_panel"
--
-- Requires RAWIMPORT_API_URL / API_TOKEN env vars.
-- NOTE: dt.gui.action only works while the target image is open in the
-- darkroom view; Apply preflights this and prints guidance otherwise.

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

-- Synchronous analysis: single GET, no polling (PRD v2.3.0 §8).
local function fetch_cct(seq, method)
    local base = api_base_url()
    if base == "" then return nil, nil, nil, "RAWIMPORT_API_URL not set" end
    local cmd = string.format(
        'curl -s%s "%s/api/v1/cct/analyze?sequence=%s&method=%s"',
        curl_base_args(), base, seq, method)
    local pipe = io.popen(cmd)
    if not pipe then return nil, nil, nil, "curl failed" end
    local out = pipe:read("*a")
    pipe:close()

    local err = out:match('"error"%s*:%s*"([^"]+)"')
    if err then return nil, nil, nil, err end

    local cct  = tonumber(out:match('"cct"%s*:%s*([%d%.]+)'))
    local tint = tonumber(out:match('"tint"%s*:%s*([%d%.%-]+)'))
    -- gains:[r,g,b]
    local g = out:match('"gains"%s*:%s*%[([^%]]*)%]')
    local gr, gg, gb
    if g then
        gr = tonumber(g:match("([^,]+)"))
        gg, gb = tonumber(g:match(",([^,]+),")), tonumber(g:match(",([^,]+)$"))
    end
    if cct and gr then
        return { cct = cct, tint = tint, r = gr, g = gg, b = gb }, nil
    end
    return nil, nil, nil, "unexpected response"
end

local METHODS = { "grayworld", "whitepatch" }

local cct_method_combo = dt.new_widget("combobox"){
    label = "Method", value = 1,
    table.unpack(METHODS),
}
local cct_status_lbl = dt.new_widget("label"){ label = "Status: ready", selectable = true }
local cct_result_lbl = dt.new_widget("label"){ label = "Result: --", selectable = true }

-- Preflight (D7): applying gains via dt.gui.action requires the target image
-- open in the darkroom view; otherwise it silently no-ops.
local function darkroom_ready_for(image)
    if dt.gui.current_view() ~= dt.gui.views.darkroom then
        return false, "Switch to darkroom view to apply gains."
    end
    local dr = dt.gui.get_darkroom_image()
    if not dr or dr.id ~= image.id then
        return false, "This image is not the one open in the darkroom."
    end
    return true
end

local analyze_btn = dt.new_widget("button"){
    label = "Analyze white balance",
    clicked_callback = function()
        local images = dt.gui.selection()
        if not images or #images == 0 then
            dt.print("CCT: no images selected")
            return
        end
        local img = images[1]
        local seq = resolve_sequence(img)
        local method = METHODS[cct_method_combo.selected]
        cct_status_lbl.label = "Status: analyzing " .. seq .. " (" .. method .. ")..."
        local res, err = fetch_cct(seq, method)
        if not res then
            cct_status_lbl.label = "Status: failed - " .. tostring(err)
            return
        end
        cct_status_lbl.label = string.format(
            "Status: done (%.0fK)", res.cct)
        cct_result_lbl.label = string.format(
            "Result: %.0fK | tint %.2f | gains [%.3f, %.3f, %.3f]",
            res.cct, res.tint, res.r, res.g, res.b)
    end,
}

local apply_btn = dt.new_widget("button"){
    label = "Apply to Color Calibration",
    clicked_callback = function()
        local images = dt.gui.selection()
        if not images or #images == 0 then return end
        local img = images[1]

        local seq = resolve_sequence(img)
        local method = METHODS[cct_method_combo.selected]
        local res, err = fetch_cct(seq, method)
        if not res then
            cct_result_lbl.label = "Result: failed - " .. tostring(err)
            return
        end

        local ok, why = darkroom_ready_for(img)
        if not ok then
            cct_result_lbl.label = "Result: " .. why
            dt.print("CCT: " .. why)
            return
        end

        -- Apply per-channel gains through the RGB channel mixer.
        pcall(function()
            dt.gui.action("iop/channelmixerrgb/red",   0, "", "set:" .. tostring(res.r), 1.0)
            dt.gui.action("iop/channelmixerrgb/green", 0, "", "set:" .. tostring(res.g), 1.0)
            dt.gui.action("iop/channelmixerrgb/blue",  0, "", "set:" .. tostring(res.b), 1.0)
        end)
        dt.print(string.format("Applied CCT %.0fK (gains %.3f/%.3f/%.3f)",
                               res.cct, res.r, res.g, res.b))
        cct_result_lbl.label = string.format(
            "Result: applied %.0fK", res.cct)
    end,
}

local cct_panel = dt.new_widget("box"){
    orientation = "vertical",
    dt.new_widget("label"){ label = "CCT Analysis", selectable = true },
    dt.new_widget("label"){ label = "Native white-balance estimation", selectable = true },
    dt.new_widget("label"){ label = " " },
    dt.new_widget("label"){ label = "Method:", selectable = true },
    cct_method_combo,
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
