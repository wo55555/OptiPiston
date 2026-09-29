add_rules("mode.debug", "mode.release")

add_repositories("levimc-repo https://github.com/LiteLDev/xmake-repo.git")

-- One DLL per Minecraft version; each value maps to a src/platform/mcXXXX adapter.
local platforms = {
    ["26.20"] = "mc2620",
}

option("mc")
    set_default("26.20")
    set_showmenu(true)
    set_description("Target Minecraft version")
    set_values("26.20")
option_end()

-- levibuildscript reads target_type for the prelink target and the manifest platform.
option("target_type")
    set_default("client")
    set_showmenu(true)
    set_values("client")
option_end()

local mc_version = get_config("mc") or "26.20"
local platform_dir = platforms[mc_version]
if not platform_dir then
    raise("OptiPiston has no platform adapter for Minecraft " .. mc_version)
end

add_requires("levilamina " .. mc_version .. ".*", {configs = {target_type = get_config("target_type")}})
add_requires("levibuildscript")
add_requires("doctest 2.4.12")

if not has_config("vs_runtime") then
    set_runtimes("MD")
end

-- git_tag is resolved in script scope, where try is available.
local get_version = function(os, git_tag)
    local version_override     = os.getenv("OPTIPISTON_VERSION")
    local has_version_override = version_override and version_override:match("%S") ~= nil
    local tag                  = has_version_override and version_override or git_tag
    tag                        = (tag or ""):gsub("^%s+", ""):gsub("%s+$", "")

    local major, minor, patch, suffix = tag:match("^v?(%d+)%.(%d+)%.(%d+)(.*)$")
    if not major then
        local ci = os.getenv("CI")
        if has_version_override or ci == "true" or ci == "1" then
            os.raise("Unable to parse OptiPiston version tag: " .. (tag ~= "" and tag or "<empty>"))
        end
        print("Failed to parse version tag, using 0.0.0")
        return "0.0.0"
    end

    if suffix ~= "" then return major .. "." .. minor .. "." .. patch .. suffix end
    return major .. "." .. minor .. "." .. patch
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
        set_toolchains("clang-cl")
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
    add_files("src/mod/**.cpp", "src/platform/" .. platform_dir .. "/**.cpp")
    add_includedirs("src", "include")
    on_load(function (target)
        -- A repo without commits or tags makes git describe fail.
        local git_tag = try { function () return os.iorun("git describe --tags --abbrev=0 --always") end }
        target:add("rules", "@levibuildscript/modpacker", {
            modVersion = get_version(os, git_tag),
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
