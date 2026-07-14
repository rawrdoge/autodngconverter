--[[
Embed Preview HUD - DNG SDK + ExifTool Hybrid
Uses DNG SDK when available for proper IFD hierarchy preservation.
Falls back to ExifTool for basic embedding.
Darktable 5.6 / Lua API 9.7.0 compatible.
]]

local dt = require "darktable"

local script_data = {
  name = "Embed Preview",
  module = "embed_preview_hud",
  destroy = function() end,
  restart = function() end
}

-- Preferences
pcall(function()
  dt.preferences.register("embed_preview_hud", "exiftool_path", "string",
    "ExifTool path", "Full path to exiftool.exe", "")
end)

pcall(function()
  dt.preferences.register("embed_preview_hud", "dng_sdk_path", "string",
    "DNG SDK tool path", "Full path to dng_preview_embed.exe", "")
end)

pcall(function()
  dt.preferences.register("embed_preview_hud", "dnglab_path", "string",
    "dnglab tool path", "Full path to dnglab.exe", "")
end)

pcall(function()
  dt.preferences.register("embed_preview_hud", "worker", "string",
    "Re-embed worker", "exiftool|dnglab|dng_sdk", "exiftool")
end)

pcall(function()
  dt.preferences.register("embed_preview_hud", "embed_mode", "string",
    "Embed mode", "adobe|preserve|minimal", "adobe")
end)

pcall(function()
  dt.preferences.register("embed_preview_hud", "auto_reeembed_on_export", "bool",
    "Auto re-embed on export", "Re-embed exported JPEG into the matching DNG after any Darktable export", false)
end)

-- ---------------------------------------------------------------------------
-- Read export module settings
-- ---------------------------------------------------------------------------
local function read_export_settings()
  local s = {width = 2048, height = 2048, quality = 90, format = "jpeg"}
  local ok, val

  ok, val = pcall(function()
    return dt.preferences.read("plugins/lighttable/export", "max_width", "integer")
  end)
  if ok and val and val > 0 then s.width = val end

  ok, val = pcall(function()
    return dt.preferences.read("plugins/lighttable/export", "max_height", "integer")
  end)
  if ok and val and val > 0 then s.height = val end

  ok, val = pcall(function()
    return dt.preferences.read("plugins/lighttable/export", "format_name", "string")
  end)
  if ok and val and val ~= "" then s.format = val end

  ok, val = pcall(function()
    return dt.preferences.read("plugins/imageio/format/jpeg", "quality", "integer")
  end)
  if ok and val and val > 0 then s.quality = val end

  return s
end

-- ---------------------------------------------------------------------------
-- Tool detection
-- ---------------------------------------------------------------------------
local function get_tool(name)
  if name == "exiftool" then
    local p = dt.preferences.read("embed_preview_hud", "exiftool_path", "string")
    if p and p ~= "" then return p end
    return "exiftool"
  elseif name == "dng_sdk" then
    local p = dt.preferences.read("embed_preview_hud", "dng_sdk_path", "string")
    if p and p ~= "" then return p end
    return "dng_preview_embed"
  elseif name == "dnglab" then
    local p = dt.preferences.read("embed_preview_hud", "dnglab_path", "string")
    if p and p ~= "" then return p end
    return "dnglab"
  end
  return name
end

local function dnglab_available()
  local path = get_tool("dnglab")
  local cmd = path:find(" ", 1, true)
    and ('"' .. path .. '" reembed --help 2>nul')
    or (path .. ' reembed --help 2>nul')
  local pipe = io.popen(cmd)
  if not pipe then return false end
  local output = pipe:read("*a")
  pipe:close()
  return output and output:match("re-embed")
end

local function tool_exists(path)
  if not path or path == "" then return false end
  local cmd = path:find(" ", 1, true)
    and ('"' .. path .. '" --version 2>nul')
    or (path .. ' --version 2>nul')
  local pipe = io.popen(cmd)
  if not pipe then return false end
  local output = pipe:read("*line")
  pipe:close()
  return output and (output:match("dng_preview_embed") or output:match("ExifTool"))
end

local function dng_sdk_available()
  local path = get_tool("dng_sdk")
  -- Check without --version since our tool might not have that flag
  local cmd = path:find(" ", 1, true)
    and ('"' .. path .. '" 2>nul')
    or (path .. ' 2>nul')
  local pipe = io.popen(cmd)
  if not pipe then return false end
  local output = pipe:read("*a")
  pipe:close()
  -- If it prints usage (contains "Usage:"), it's available
  return output and output:match("Usage")
end

local function file_writable(filepath)
  local f = io.open(filepath, "r+b")
  if f then f:close(); return true end
  return false
end

local function check_writable_status(images)
  local writable, total = 0, 0
  if not images then return 0, 0 end
  for _, img in ipairs(images) do
    if img.filename:lower():match("%.dng$") then
      total = total + 1
      if file_writable(img.path .. "/" .. img.filename) then
        writable = writable + 1
      end
    end
  end
  return writable, total
end

-- Verify DNG is still valid
local function verify_dng(tool, dng_path)
  local cmd = tool:find(" ", 1, true)
    and ('"' .. tool .. '" -FileType -Error -Warning "' .. dng_path .. '" 2>nul')
    or (tool .. ' -FileType -Error -Warning "' .. dng_path .. '" 2>nul')
  local pipe = io.popen(cmd)
  if not pipe then return false, "cannot verify" end
  local output = pipe:read("*a")
  pipe:close()
  if output:match("Error") or output:match("Corrupt") then
    return false, output
  end
  return true, output
end

-- ---------------------------------------------------------------------------
-- UI state
-- ---------------------------------------------------------------------------
local status_lbl, sel_lbl, result_lbl, export_status_lbl, writable_lbl, sdk_status_lbl
local mode_combo, custom_ent, sdk_ent, width_ent, height_ent, quality_ent
local preset_combo, mode_sdk_combo

local function update_status()
  local exif_path = get_tool("exiftool")
  local sdk_path = get_tool("dng_sdk")

  if status_lbl then
    status_lbl.label = tool_exists(exif_path) and ("ExifTool: OK (" .. exif_path .. ")") or ("ExifTool: NOT FOUND (" .. exif_path .. ")")
  end

  if sdk_status_lbl then
    local sdk_ok = dng_sdk_available()
    sdk_status_lbl.label = sdk_ok and ("DNG SDK: OK (" .. sdk_path .. ")") or ("DNG SDK: NOT FOUND (" .. sdk_path .. ")")
  end

  local images = dt.gui.selection()
  local count = 0
  if images then
    for _, img in ipairs(images) do
      if img.filename:lower():match("%.dng$") then count = count + 1 end
    end
  end
  if sel_lbl then sel_lbl.label = "Selected: " .. count .. " DNG(s)" end

  local w, t = check_writable_status(images)
  if writable_lbl then
    writable_lbl.label = (t == 0) and "Writable: 0/0 (no DNGs)" or ("Writable: " .. w .. "/" .. t .. " DNGs")
  end
end

local function sync_export_settings()
  local s = read_export_settings()
  if width_ent then width_ent.text = tostring(s.width) end
  if height_ent then height_ent.text = tostring(s.height) end
  if quality_ent then quality_ent.text = tostring(s.quality) end

  if export_status_lbl then
    export_status_lbl.label = string.format("↳ Export module: %dx%d %s, Q%d", s.width, s.height, s.format:upper(), s.quality)
  end
end

-- ---------------------------------------------------------------------------
-- Apply preset
-- ---------------------------------------------------------------------------
local function apply_preset()
  if preset_combo.selected == 2 then  -- Windows Compatible
    width_ent.text = "1024"
    height_ent.text = "1024"
    quality_ent.text = "85"
    if export_status_lbl then
      export_status_lbl.label = "↳ Preset: Windows Compatible (1024x1024, Q85)"
    end
  elseif preset_combo.selected == 3 then  -- Full Size
    width_ent.text = "2048"
    height_ent.text = "2048"
    quality_ent.text = "90"
    if export_status_lbl then
      export_status_lbl.label = "↳ Preset: Full Size (2048x2048, Q90)"
    end
  elseif preset_combo.selected == 4 then  -- Adobe Compatible
    width_ent.text = "4000"
    height_ent.text = "3000"
    quality_ent.text = "92"
    if export_status_lbl then
      export_status_lbl.label = "↳ Preset: Adobe Compatible (4000x3000, Q92) — Multi-res pyramid"
    end
  else
    sync_export_settings()
  end
end

-- ---------------------------------------------------------------------------
-- Export image at specific dimensions
-- ---------------------------------------------------------------------------
local function export_image(img, tmp_path, max_w, max_h, qual)
  return pcall(function()
    local fmt = dt.new_format("jpeg")
    fmt.max_width = max_w
    fmt.max_height = max_h
    fmt.quality = qual
    fmt:write_image(img, tmp_path)
  end)
end

-- ---------------------------------------------------------------------------
-- Embed using DNG SDK
-- ---------------------------------------------------------------------------
local function embed_with_sdk(dng, tmp, options)
  local sdk = get_tool("dng_sdk")
  local mode = dt.preferences.read("embed_preview_hud", "embed_mode", "string") or "adobe"

  local cmd = sdk:find(" ", 1, true)
    and ('"' .. sdk .. '" "' .. dng .. '" "' .. tmp .. '"')
    or (sdk .. ' "' .. dng .. '" "' .. tmp .. '"')

  if mode == "adobe" then
    cmd = cmd .. ' --adobe-compatible'
  elseif mode == "preserve" then
    cmd = cmd .. ' --preserve-hierarchy'
  end

  if options.preview_w and options.preview_h then
    cmd = cmd .. string.format(' --preview-size %dx%d', options.preview_w, options.preview_h)
  end

  cmd = cmd .. ' --verbose'

  local r = os.execute(cmd)
  return (r == true or r == 0)
end

-- ---------------------------------------------------------------------------
-- Embed using ExifTool (fallback)
-- ---------------------------------------------------------------------------
local function embed_with_exiftool(tool, dng, tmp, is_adobe)
  local quote = tool:find(" ", 1, true) and '"' or ''
  local tool_quoted = quote .. tool .. quote

  -- Phase 1: Delete old previews
  local del_cmd = string.format(
    '%s -m -P -a -PreviewImage= -JpgFromRaw= -ThumbnailImage= -overwrite_original_in_place "%s"',
    tool_quoted, dng
  )
  os.execute(del_cmd)

  -- Phase 2: Write new preview
  local write_cmd = tool_quoted .. " -m -P -a"

  if is_adobe then
    -- Multi-resolution: write to both tags
    write_cmd = write_cmd .. string.format(' "-PreviewImage<=%s"', tmp)
    write_cmd = write_cmd .. string.format(' "-JpgFromRaw<=%s"', tmp)
  else
    write_cmd = write_cmd .. string.format(' "-PreviewImage<=%s"', tmp)
  end

  -- Add Adobe-compatible metadata
  write_cmd = write_cmd .. ' -PreviewApplicationName="Adobe Photoshop Camera Raw"'
  write_cmd = write_cmd .. ' -PreviewApplicationVersion="16.5"'
  write_cmd = write_cmd .. ' -PreviewColorSpace=sRGB'
  write_cmd = write_cmd .. ' -PreviewDateTime="' .. os.date("%Y:%m:%d %H:%M:%S") .. '"'

  write_cmd = write_cmd .. string.format(' -overwrite_original_in_place "%s"', dng)

  local r = os.execute(write_cmd)
  return (r == true or r == 0)
end

-- ---------------------------------------------------------------------------
-- Embed using dnglab `reembed` (native multi-res SubIFD1/SubIFD2 preview)
-- Re-encodes the whole DNG (raw preserved losslessly, preview replaced).
-- ---------------------------------------------------------------------------
local function embed_with_dnglab(dng, tmp)
  local lab = get_tool("dnglab")
  -- dnglab reembed overwrites --output in place via temp+rename, so we point
  -- --output at the same file. A deterministic --seed keeps output stable.
  local cmd = lab:find(" ", 1, true)
    and string.format('"%s" reembed --dng "%s" --preview "%s" --output "%s" --seed "%s"',
                      lab, dng, tmp, dng, "preview-edit")
    or string.format('%s reembed --dng "%s" --preview "%s" --output "%s" --seed "%s"',
                      lab, dng, tmp, dng, "preview-edit")
  local r = os.execute(cmd)
  return (r == true or r == 0)
end

-- ---------------------------------------------------------------------------
-- Notify the RawImport API that a preview was re-embedded (PRD Q8).
-- Updates the DB output_hash so the corruption monitor doesn't false-positive.
-- Reads RAWIMPORT_API_URL (and optional API_TOKEN) from the environment.
-- ---------------------------------------------------------------------------
local function notify_preview_updated(dng, worker, w, h, q)
  local base = os.getenv("RAWIMPORT_API_URL")
  if not base or base == "" then return end
  local token = os.getenv("API_TOKEN") or ""
  local body = string.format(
    '{"output_path":"%s","worker":"%s","preview_width":%d,"preview_height":%d,"preview_quality":%d}',
    dng, worker, w or 0, h or 0, q or 0)
  local auth_hdr = (token ~= "") and (' -H "Authorization: Bearer ' .. token .. '"') or ""
  local cmd = string.format('curl -s -o nul -w "%%{http_code}" -X POST %s%s -H "Content-Type: application/json" -d "%s"',
    auth_hdr, base .. "/api/v1/imports/by-path/preview-updated", body)
  local pipe = io.popen(cmd)
  if pipe then
    local code = pipe:read("*a")
    pipe:close()
    if code and code:match("200") then
      dt.print("Notified API: preview hash updated")
    else
      dt.print("WARN: API notify failed (" .. tostring(code) .. ")")
    end
  end
end

-- ---------------------------------------------------------------------------
-- Resolve the matching DNG for a source RAW via the RawImport API.
-- Returns the DNG output_path (string) or nil. Uses GET /api/v1/imports/by-source.
-- ---------------------------------------------------------------------------
local function resolve_dng_via_api(source_path)
  local base = os.getenv("RAWIMPORT_API_URL")
  if not base or base == "" then return nil end
  local token = os.getenv("API_TOKEN") or ""
  local esc = source_path:gsub('"', '\\"')
  local auth_hdr = (token ~= "") and (' -H "Authorization: Bearer ' .. token .. '"') or ""
  local cmd = string.format('curl -s %s "%s/api/v1/imports/by-source?path=%s"',
    auth_hdr, base, esc:gsub(" ", "%%20"))
  local pipe = io.popen(cmd)
  if not pipe then return nil end
  local out = pipe:read("*a")
  pipe:close()
  if not out or out == "" then return nil end
  -- Extract output_path from the JSON (lightweight parse, no dependency).
  local op = out:match('"output_path"%s*:%s*"([^"]+)"')
  return op
end

-- ---------------------------------------------------------------------------
-- Export-hook: after Darktable exports an image, if its source is a DNG in the
-- RawImport library, re-embed the just-exported JPEG into that DNG. This is the
-- "share my edit back into the DNG" flow (Immich extracts embedded JPEGs).
-- Gated by the `auto_reeembed_on_export` preference (default off).
-- ---------------------------------------------------------------------------
local function on_post_export(image, session, exported_path)
  local enabled = dt.preferences.read("embed_preview_hud", "auto_reeembed_on_export", "bool")
  if not enabled then return end
  if not exported_path or exported_path == "" then return end
  if not exported_path:lower():match("%.jpe?g$") then return end

  -- The source RAW path is what the API indexed (image.path + filename).
  local source_path = image.path .. "/" .. image.filename
  local dng = resolve_dng_via_api(source_path)
  if not dng or dng == "" then
    dt.print("Auto re-embed: no matching DNG for " .. image.filename)
    return
  end
  if not file_writable(dng) then
    dt.print("Auto re-embed: DNG not writable: " .. dng)
    return
  end

  local worker = dt.preferences.read("embed_preview_hud", "worker", "string") or "exiftool"
  local tool, worker_label
  if worker == "dnglab" then
    if not dnglab_available() then return end
    tool = get_tool("dnglab"); worker_label = "dnglab"
  elseif worker == "dng_sdk" then
    if not dng_sdk_available() then return end
    tool = get_tool("dng_sdk"); worker_label = "DNG SDK"
  else
    if not tool_exists(get_tool("exiftool")) then return end
    tool = get_tool("exiftool"); worker_label = "ExifTool"
  end

  local is_adobe = (dt.preferences.read("embed_preview_hud", "embed_mode", "string") or "adobe") == "adobe"
  local s = read_export_settings()

  local embed_ok
  if worker == "dnglab" then
    embed_ok = embed_with_dnglab(dng, exported_path)
  elseif worker == "dng_sdk" then
    embed_ok = embed_with_sdk(dng, exported_path, {preview_w = s.width, preview_h = s.height})
  else
    embed_ok = embed_with_exiftool(tool, dng, exported_path, is_adobe)
  end

  if embed_ok then
    local exif_tool = get_tool("exiftool")
    local valid = verify_dng(exif_tool, dng)
    if valid then
      pcall(function() os.remove(dng .. "_original") end)
      notify_preview_updated(dng, worker, s.width, s.height, s.quality)
      dt.print("Auto re-embedded edit into " .. dng .. " (" .. worker_label .. ")")
    else
      local backup = dng .. "_original"
      local bf = io.open(backup, "rb")
      if bf then bf:close(); os.execute('move /Y "' .. backup .. '" "' .. dng .. '"') end
      dt.print("WARN: re-embed failed verification, restored backup for " .. dng)
    end
  else
    dt.print("WARN: re-embed failed for " .. dng)
  end
end

-- ---------------------------------------------------------------------------
-- Core worker
-- ---------------------------------------------------------------------------
local function do_embed()
  local images = dt.gui.selection()
  if not images or #images == 0 then
    if result_lbl then result_lbl.label = "Error: No images selected" end
    return
  end

  local worker = dt.preferences.read("embed_preview_hud", "worker", "string") or "exiftool"

  -- Resolve the worker + its tool path
  local tool, worker_label
  if worker == "dnglab" then
    if not dnglab_available() then
      if result_lbl then result_lbl.label = "Error: dnglab not found (set dnglab_path)" end
      return
    end
    tool = get_tool("dnglab")
    worker_label = "dnglab"
  elseif worker == "dng_sdk" then
    if not dng_sdk_available() then
      if result_lbl then result_lbl.label = "Error: DNG SDK not found (set dng_sdk_path)" end
      return
    end
    tool = get_tool("dng_sdk")
    worker_label = "DNG SDK"
  else
    if not tool_exists(get_tool("exiftool")) then
      if result_lbl then result_lbl.label = "Error: ExifTool not found" end
      return
    end
    tool = get_tool("exiftool")
    worker_label = "ExifTool"
  end

  local dngs = {}
  for _, img in ipairs(images) do
    if img.filename:lower():match("%.dng$") then table.insert(dngs, img) end
  end
  if #dngs == 0 then
    if result_lbl then result_lbl.label = "Error: No DNGs selected" end
    return
  end

  local w, t = check_writable_status(images)
  if w < t then
    if result_lbl then result_lbl.label = "Error: " .. (t - w) .. " DNG(s) not writable" end
    return
  end

  local is_adobe = (preset_combo.selected == 4)
  local max_w = tonumber(width_ent.text) or 2048
  local max_h = tonumber(height_ent.text) or 2048
  local qual = tonumber(quality_ent.text) or 90

  if result_lbl then result_lbl.label = "0/" .. #dngs .. " (" .. worker_label .. ")" end
  local ok_c, fail_c = 0, 0

  for i, img in ipairs(dngs) do
    local dng = img.path .. "/" .. img.filename
    local tmp = img.path .. "/.embed_tmp_" .. tostring(os.time()) .. "_" .. i .. ".jpg"

    local exp_ok = export_image(img, tmp, max_w, max_h, qual)

    if not exp_ok then
      fail_c = fail_c + 1
      pcall(function() os.remove(tmp) end)
    else
      local embed_ok
      if worker == "dnglab" then
        embed_ok = embed_with_dnglab(dng, tmp)
      elseif worker == "dng_sdk" then
        embed_ok = embed_with_sdk(dng, tmp, {preview_w = max_w, preview_h = max_h})
      else
        embed_ok = embed_with_exiftool(tool, dng, tmp, is_adobe)
      end

      pcall(function() os.remove(tmp) end)

      if embed_ok then
        -- Verify
        local exif_tool = get_tool("exiftool")
        local valid, _ = verify_dng(exif_tool, dng)
        if valid then
          pcall(function() os.remove(dng .. "_original") end)
          ok_c = ok_c + 1
          -- Notify the RawImport API so the DB output_hash stays in sync (PRD Q8)
          notify_preview_updated(dng, worker, max_w, max_h, qual)
        else
          -- Restore from backup
          local backup = dng .. "_original"
          local bf = io.open(backup, "rb")
          if bf then
            bf:close()
            os.execute('move /Y "' .. backup .. '" "' .. dng .. '"')
            fail_c = fail_c + 1
            if result_lbl then
              result_lbl.label = string.format("%d/%d (%d OK, %d fail) - RESTORED", i, #dngs, ok_c, fail_c)
            end
          else
            fail_c = fail_c + 1
          end
        end
      else
        fail_c = fail_c + 1
      end
    end

    if result_lbl then
      result_lbl.label = string.format("%d/%d (%d OK, %d fail)", i, #dngs, ok_c, fail_c)
    end
  end

  if result_lbl then
    result_lbl.label = string.format("Done. OK: %d | Failed: %d (%s)", ok_c, fail_c, worker_label)
  end
  if ok_c > 0 then dt.print("DNGs modified. Re-import if skulls appear.") end
end

-- ---------------------------------------------------------------------------
-- Install panel
-- ---------------------------------------------------------------------------
local function install()
  preset_combo = dt.new_widget("combobox"){
    "Use Export Module Settings",
    "Windows Compatible (1024x1024, Q85)",
    "Full Size (2048x2048, Q90)",
    "Adobe Compatible (4000x3000, Q92) — Multi-res pyramid"
  }

  export_status_lbl = dt.new_widget("label"){
    label = "↳ Export module: reading...",
    selectable = true
  }

  status_lbl = dt.new_widget("label"){ label = "ExifTool: checking...", selectable = true }
  sdk_status_lbl = dt.new_widget("label"){ label = "DNG SDK: checking...", selectable = true }
  sel_lbl    = dt.new_widget("label"){ label = "Selected: unknown", selectable = true }
  writable_lbl = dt.new_widget("label"){ label = "Writable: unknown", selectable = true }
  result_lbl = dt.new_widget("label"){ label = "Ready", selectable = true }

  mode_combo = dt.new_widget("combobox"){
    "Auto-detect from PATH",
    "Use custom path below"
  }

  local mode_lbl = dt.new_widget("label"){ label = "ExifTool source:", selectable = true }

  custom_ent = dt.new_widget("entry"){
    tooltip = "Full path to exiftool.exe",
    text = dt.preferences.read("embed_preview_hud", "exiftool_path", "string") or "",
    editable = true
  }

  local custom_lbl = dt.new_widget("label"){ label = "Custom path:", selectable = true }

  -- DNG SDK mode
  mode_sdk_combo = dt.new_widget("combobox"){
    "Auto-detect from PATH",
    "Use custom path below"
  }

  local mode_sdk_lbl = dt.new_widget("label"){ label = "DNG SDK source:", selectable = true }

  sdk_ent = dt.new_widget("entry"){
    tooltip = "Full path to dng_preview_embed.exe",
    text = dt.preferences.read("embed_preview_hud", "dng_sdk_path", "string") or "",
    editable = true
  }

  local sdk_custom_lbl = dt.new_widget("label"){ label = "Custom SDK path:", selectable = true }

  -- Worker selector (which re-embed engine to use)
  local worker_lbl = dt.new_widget("label"){ label = "Re-embed worker:", selectable = true }
  worker_combo = dt.new_widget("combobox"){
    "ExifTool (default, fast tag-swap)",
    "dnglab reembed (native multi-res)",
    "DNG SDK (C++ preserve)"
  }

  -- dnglab path
  local mode_lab_combo = dt.new_widget("combobox"){
    "Auto-detect from PATH",
    "Use custom path below"
  }
  local lab_lbl = dt.new_widget("label"){ label = "dnglab source:", selectable = true }
  lab_ent = dt.new_widget("entry"){
    tooltip = "Full path to dnglab.exe",
    text = dt.preferences.read("embed_preview_hud", "dnglab_path", "string") or "",
    editable = true
  }
  local lab_custom_lbl = dt.new_widget("label"){ label = "Custom dnglab path:", selectable = true }

  width_ent = dt.new_widget("entry"){ text = "2048", editable = true }
  local width_lbl = dt.new_widget("label"){ label = "Max width:", selectable = true }

  height_ent = dt.new_widget("entry"){ text = "2048", editable = true }
  local height_lbl = dt.new_widget("label"){ label = "Max height:", selectable = true }

  quality_ent = dt.new_widget("entry"){ text = "90", editable = true }
  local quality_lbl = dt.new_widget("label"){ label = "JPEG quality (10-100):", selectable = true }

  local preset_lbl = dt.new_widget("label"){ label = "Compatibility preset:", selectable = true }

  local preset_btn = dt.new_widget("button"){
    label = "Apply Preset",
    clicked_callback = apply_preset
  }

  local refresh_btn = dt.new_widget("button"){
    label = "Refresh Status & Sync Export",
    clicked_callback = function()
      if mode_combo.selected == 2 then
        dt.preferences.write("embed_preview_hud", "exiftool_path", "string", custom_ent.text)
      else
        dt.preferences.write("embed_preview_hud", "exiftool_path", "string", "")
      end
      if mode_sdk_combo.selected == 2 then
        dt.preferences.write("embed_preview_hud", "dng_sdk_path", "string", sdk_ent.text)
      else
        dt.preferences.write("embed_preview_hud", "dng_sdk_path", "string", "")
      end
      if mode_lab_combo.selected == 2 then
        dt.preferences.write("embed_preview_hud", "dnglab_path", "string", lab_ent.text)
      else
        dt.preferences.write("embed_preview_hud", "dnglab_path", "string", "")
      end
      -- Persist selected worker
      local wsel = worker_combo.selected
      local wval = (wsel == 2) and "dnglab" or (wsel == 3) and "dng_sdk" or "exiftool"
      dt.preferences.write("embed_preview_hud", "worker", "string", wval)
      update_status()
      if preset_combo.selected == 1 then sync_export_settings() end
    end
  }

  local embed_btn = dt.new_widget("button"){
    label = "Export & Embed Previews",
    clicked_callback = do_embed
  }

  local panel = dt.new_widget("box"){
    orientation = "vertical",
    dt.new_widget("label"){ label = "Embed Edited Preview", selectable = true },
    dt.new_widget("label"){ label = "Replaces DNG embedded preview with current edit", selectable = true },
    dt.new_widget("label"){ label = " " },
    preset_lbl,
    preset_combo,
    preset_btn,
    dt.new_widget("label"){ label = " " },
    export_status_lbl,
    dt.new_widget("label"){ label = " " },
    status_lbl,
    sdk_status_lbl,
    sel_lbl,
    writable_lbl,
    dt.new_widget("label"){ label = " " },
    mode_lbl,
    mode_combo,
    custom_lbl,
    custom_ent,
    dt.new_widget("label"){ label = " " },
    mode_sdk_lbl,
    mode_sdk_combo,
    sdk_custom_lbl,
    sdk_ent,
    dt.new_widget("label"){ label = " " },
    worker_lbl,
    worker_combo,
    lab_lbl,
    mode_lab_combo,
    lab_custom_lbl,
    lab_ent,
    refresh_btn,
    dt.new_widget("label"){ label = " " },
    dt.new_widget("label"){ label = "Export settings (synced from Export module):", selectable = true },
    width_lbl,
    width_ent,
    height_lbl,
    height_ent,
    quality_lbl,
    quality_ent,
    dt.new_widget("label"){ label = " " },
    embed_btn,
    result_lbl
  }

  local views = dt.gui.views
  if not views or not views.lighttable then
    dt.print_error("embed_preview_hud: dt.gui.views.lighttable is nil")
    return
  end

  local reg_ok, reg_err = pcall(function()
    dt.register_lib(
      "embed_preview_hud",
      "Embed Preview",
      true,
      true,
      {
        [views.lighttable] = {"DT_UI_CONTAINER_PANEL_RIGHT_CENTER", 100}
      },
      panel
    )
  end)

  if not reg_ok then
    reg_ok, reg_err = pcall(function()
      dt.register_lib(
        "embed_preview_hud",
        "Embed Preview",
        true,
        {
          [views.lighttable] = {"DT_UI_CONTAINER_PANEL_RIGHT_CENTER", 100}
        },
        panel
      )
    end)
  end

  if reg_ok then
    dt.print("Embed Preview HUD registered. Enable it via 'more modules' in Lighttable.")
    print("[embed_preview_hud] Registered successfully.")
  else
    dt.print_error("embed_preview_hud: register_lib failed: " .. tostring(reg_err))
    print("[embed_preview_hud] register_lib failed: " .. tostring(reg_err))
    return
  end

  -- Register the export-hook so a Darktable export can auto re-embed the
  -- edited JPEG back into the matching DNG (Immich "share thumbnail" flow).
  -- Gated by the `auto_reeembed_on_export` preference (default off).
  pcall(function()
    dt.register_event("post-export-image", on_post_export)
    print("[embed_preview_hud] post-export-image hook registered.")
  end)

  update_status()
  sync_export_settings()
end

-- ---------------------------------------------------------------------------
-- Safe startup
-- ---------------------------------------------------------------------------
local ok, err = pcall(install)
if not ok then
  dt.print_error("embed_preview_hud: " .. tostring(err))
  print("[embed_preview_hud] Startup error: " .. tostring(err))
end

script_data.restart = install
return script_data
