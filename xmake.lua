add_rules("mode.debug", "mode.release")

add_repositories("levimc-repo https://github.com/LiteLDev/xmake-repo.git")

local optipiston_version = "0.1.2"

-- One DLL per release line; code is exposed to the adapter as OPTIPISTON_MC.
local mc_lines = {
    ["26.10"] = {levilamina = "26.10", code = 2610},
    ["26.20"] = {levilamina = "26.20", code = 2620},
    ["26.30"] = {levilamina = "26.32", code = 2632},
    ["26.40"] = {levilamina = "26.40", code = 2640},
    ["26.50"] = {levilamina = "26.51", code = 2651},
}

option("mc")
    set_default("26.20")
    set_showmenu(true)
    set_description("Target Minecraft release line")
    set_values("26.10", "26.20", "26.30", "26.40", "26.50")
option_end()

-- levibuildscript reads target_type for the prelink target and the manifest platform.
option("target_type")
    set_default("client")
    set_showmenu(true)
    set_values("client")
option_end()

local mc_version = get_config("mc") or "26.20"
local mc_line = mc_lines[mc_version]
if not mc_line then
    raise("OptiPiston has no platform adapter for Minecraft " .. mc_version)
end
local mod_version = optipiston_version .. "-mc" .. mc_version
local levilamina_range = mc_line.levilamina .. ".*"

add_requires("levilamina " .. levilamina_range, {configs = {target_type = get_config("target_type")}})
add_requires("levibuildscript")
add_requires("doctest 2.4.12")

if not has_config("vs_runtime") then
    set_runtimes("MD")
end

-- Lip reads tooth.json from the release tag, so keep it in sync with --mc.
local function sync_tooth(io, os, path)
    local file = path.join(os.projectdir(), "tooth.json")
    local text = io.readfile(file)
    local synced = text
        :gsub('("version"%s*:%s*)"[^"]*"', '%1"' .. mod_version .. '"', 1)
        :gsub('("github%.com/LiteLDev/LeviLamina#client"%s*:%s*)"[^"]*"', '%1"' .. levilamina_range .. '"', 1)
    if synced ~= text then
        io.writefile(file, synced)
        print("tooth.json updated to " .. mod_version .. " (LeviLamina " .. levilamina_range .. ")")
    end
end

local function windows_flags()
    if is_plat("windows") then
        add_defines("NOMINMAX", "UNICODE")
        set_exceptions("none") -- To avoid conflicts with /EHa.
        add_cxflags("/EHa", "/utf-8", "/W4", "/w44265", "/w44289", "/w44296", "/w45263", "/w44738", "/w45204")
        add_cxflags(
            "/EHs",
            "-Wno-microsoft-cast",
            "-Wno-invalid-offsetof",
            "-Wno-c++2b-extensions",
            "-Wno-microsoft-include",
            "-Wno-overloaded-virtual",
            "-Wno-ignored-qualifiers",
            "-Wno-missing-field-initializers",
            "-Wno-potentially-evaluated-expression",
            "-Wno-pragma-system-header-outside-header",
            {tools = {"clang_cl"}}
        )
        -- The rapidjson bundled with LeviLamina 26.10 does not compile under current clang.
        set_toolchains(mc_version == "26.10" and "msvc" or "clang-cl")
    end
    set_languages("c++20")
    set_symbols("debug")
end

-- Pure C++ logic with no Minecraft or LeviLamina headers.
target("optipiston-core")
    set_kind("static")
    windows_flags()
    add_headerfiles("src/core/**.h")
    add_files("src/core/**.cpp")
    add_includedirs("src", {public = true})

target("OptiPiston")
    add_rules("@levibuildscript/linkrule")
    windows_flags()
    add_deps("optipiston-core")
    add_packages("levilamina")
    set_kind("shared")
    if is_mode("debug") then
        add_defines("DEBUG")
    end
    add_headerfiles("src/mod/**.h", "src/platform/*.h", "include/optipiston/*.h", "include/optipiston/*.hpp")
    add_files("src/mod/**.cpp", "src/platform/bedrock/**.cpp")
    add_includedirs("src", "include")
    add_defines("OPTIPISTON_MC=" .. mc_line.code)
    on_load(function (target)
        sync_tooth(io, os, path)
        target:add("rules", "@levibuildscript/modpacker", {
            modVersion = mod_version,
        })
    end)
    after_build(function (target)
        local output_dir = path.join(os.projectdir(), "bin", target:name())
        os.mkdir(output_dir)
        os.cp(path.join(os.projectdir(), "LICENSE"), output_dir)

        local lang_dir = path.join(output_dir, "lang")
        os.tryrm(lang_dir)
        os.cp(path.join(os.projectdir(), "src", "lang"), lang_dir)
    end)

target("optipiston-tests")
    set_kind("binary")
    set_default(false)
    windows_flags()
    add_deps("optipiston-core")
    add_packages("doctest")
    add_files("tests/**.cpp")
