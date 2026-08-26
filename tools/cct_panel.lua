-- cct_panel.lua — CCT Analysis panel for darktable (PRD v2.3.0 §8).
-- Queries the synchronous native endpoint GET /api/v1/cct/analyze and applies
-- the returned white-balance gains to the Color Calibration module.
--
-- Install: copy into darktable's lua config dir and require from luarc:
--   require "cct_panel"

-- §8.1: API URL from env, falling back to localhost default.
local api_url = os.getenv("RAWIMPORT_API_URL") or "http://localhost:8080"
local api_token = os.getenv("API_TOKEN") or ""

local function curl_base_args()
    return (api_token ~= "") and (' -H "Authorization: Bearer ' .. api_token .. '"') or ""
end

local METHODS = { "shadesofgrey", "whitepatch" }

-- ---------------------------------------------------------------- widgets --
local url_entry    = dt.new_widget("entry"){ text = api_url,
    tooltip = "RawImport pipeline base URL", placeholder = "http://localhost:8080" }
local seq_entry    = dt.new_widget("entry"){ text = "",
    tooltip = "IMG_{n} sequence name" }
local method_combo = dt.new_widget("combobox"){ label = "Method", value = 1,
    table.unpack(METHODS) }
local status_lbl   = dt.new_widget("label"){ label = "Status: ready", selectable = true }
local cct_lbl      = dt.new_widget("label"){ label = "CCT: --", selectable = true }
local tint_lbl     = dt.new_widget("label"){ label = "Tint: --", selectable = true }
local gains_lbl    = dt.new_widget("label"){ label = "Gains: --", selectable = true }
local apply_btn    = dt.new_widget("button"){ label = "Apply to Color Calibration",
                                              sensitive = false }

-- Populate the sequence entry from a sole lighttable selection.
local function autofill_sequence()
    if seq_entry.text ~= "" then return end
    local images = dt.gui.selection()
    if images and #images == 1 then
        seq_entry.text = images[1].filename:gsub("%..*$", "")
    end
end

-- -------------------------------------------------------------- analysis --
-- Synchronous GET with HTTP-status capture (§8.4 error mapping).
-- Returns: body(string), code(number) or nil, errmsg.
local function http_get(path_and_query)
    local url = string.format("%s%s", url_entry.text, path_and_query)
    local cmd = string.format('curl -s -w "\\n%%{http_code}"%s "%s"',
                              curl_base_args(), url)
    local pipe = io.popen(cmd)
    if not pipe then
        return nil, "Pipeline unreachable at " .. url_entry.text ..
                    ". Check connection."
    end
    local out = pipe:read("*a")
    local ok, is_exit, exit_code = pipe:close()
    if not ok and (not out or out == "") then
        return nil, "Pipeline unreachable at " .. url_entry.text ..
                    ". Check connection."
    end
    local body = out:match("^(.*)%s*(%d%d%d)%s*$")
    if not body then return nil, "Malformed response from pipeline." end
    local code = tonumber(out:match("(%d%d%d)%s*$"))
    return body, nil, code
end

local function analyze_clicked()
    autofill_sequence()
    local seq = seq_entry.text
    if seq == "" then
        status_lbl.label = "Status: enter a sequence name"
        return
    end
    local method = METHODS[method_combo.selected]
    status_lbl.label = "Status: analyzing " .. seq .. " ..."
    cct_lbl.label, tint_lbl.label, gains_lbl.label = "CCT: --", "Tint: --", "Gains: --"
    apply_btn.sensitive = false

    local body, err, code = http_get(
        string.format("/api/v1/cct/analyze?sequence=%s&method=%s", seq, method))
    if err then
        status_lbl.label = "Status: failed"
        cct_lbl.label = "Error: " .. err
        return
    end

    local jerr = body:match('"error"%s*:%s*"([^"]+)"')
    if jerr then
        status_lbl.label = "Status: failed (" .. tostring(code) .. ")"
        if jerr == "sequence not found" then
            cct_lbl.label = "Error: Sequence not found in pipeline database."
        elseif jerr == "raw decode failed" then
            cct_lbl.label = "Error: RAW decode failed. Check that source file exists."
        else
            cct_lbl.label = "Error: " .. jerr
        end
        return
    end

    local cct  = tonumber(body:match('"cct"%s*:%s*([%d%.]+)'))
    local tint = tonumber(body:match('"tint"%s*:%s*([%d%.%-]+)'))
    local g = body:match('"gains"%s*:%s*%[([^%]]*)%]')
    local gr = tonumber(g and g:match("^%s*([^,%s]+)"))
    local gg = tonumber(g and g:match(",%s*([^,%s]+)"))
    local gb = tonumber(g and g:match(",%s*([^,%s]+)%s*$"))
    if not cct or not gr then
        status_lbl.label = "Status: malformed response"
        return
    end

    cct_lbl.label   = string.format("CCT: %.0f K", cct)
    tint_lbl.label  = string.format("Tint: %.2f", tint or 0)
    gains_lbl.label = string.format("Gains: R×%.3f  G×%.3f  B×%.3f",
                                    gr, gg or 1.0, gb or 1.0)
    status_lbl.label = "Status: completed"
    apply_btn.sensitive = true
    -- stash for Apply
    __cct_last = { r = gr, g = gg or 1.0, b = gb or 1.0, cct = cct }
end

local function apply_clicked()
    autofill_sequence()
    local images = dt.gui.selection()
    if not __cct_last then
        dt.print("CCT: run Analyze first")
        return
    end
    local img = (images and #images == 1) and images[1] or nil
    if not img and dt.gui.current_view() == dt.gui.views.darkroom then
        img = dt.gui.get_darkroom_image()
    end

    -- §8.3 preflight: darkroom view active AND target image loaded.
    if dt.gui.current_view() ~= dt.gui.views.darkroom then
        dt.print("Switch to darkroom view with the target image loaded to apply gains.")
        status_lbl.label = "Status: switch to darkroom view to apply gains."
        return
    end
    local dr = dt.gui.get_darkroom_image()
    if not dr or (img and dr.id ~= img.id) then
        dt.print("Switch to darkroom view with the target image loaded to apply gains.")
        status_lbl.label = "Status: target image not loaded in darkroom."
        return
    end

    -- PRD §8.3: push gains via the colorbalance RGB gain action.
    pcall(function()
        dt.gui.action("lib/colorbalance/rgb/gain", "set",
                      string.format("%.4f,%.4f,%.4f",
                                    __cct_last.r, __cct_last.g, __cct_last.b))
    end)
    dt.print(string.format("Applied CCT %.0fK gains (%.3f/%.3f/%.3f)",
                           __cct_last.cct, __cct_last.r,
                           __cct_last.g, __cct_last.b))
    status_lbl.label = "Status: gains applied"
end

-- ---------------------------------------------------------------- panel --
local cct_panel = dt.new_widget("box"){
    orientation = "vertical",
    dt.new_widget("label"){ label = "CCT Analysis", selectable = true },
    dt.new_widget("label"){ label = "Native white-balance estimation", selectable = true },
    dt.new_widget("label"){ label = " " },
    dt.new_widget("label"){ label = "API URL:", selectable = true },
    url_entry,
    dt.new_widget("label"){ label = " " },
    dt.new_widget("label"){ label = "Sequence:", selectable = true },
    seq_entry,
    dt.new_widget("label"){ label = "Method:", selectable = true },
    method_combo,
    dt.new_widget("label"){ label = " " },
    analyze_btn,
    cct_lbl,
    tint_lbl,
    gains_lbl,
    dt.new_widget("label"){ label = " " },
    apply_btn,
    status_lbl,
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
