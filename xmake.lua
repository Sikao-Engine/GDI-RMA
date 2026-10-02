-- GDI-RMA standalone xmake build: the ETH Zurich GDI (Graph Database
-- Interface over MPI RMA, arXiv 2305.11162) compiled as ONE shared library,
-- gdi_rma.dll / libgdi_rma.so, mirroring src/Makefile (libgdi.a, OBJS list =
-- src/*.c, 25 files, none of them contains a main()).
--
-- MPI is a SYSTEM dependency on purpose: this build declares no xrepo
-- packages (add_requires/add_packages appear nowhere), same house rule as
-- thirdparty/kimix-native.
--   Windows: MS-MPI SDK + runtime, e.g.
--     winget install --id Microsoft.msmpisdk --accept-package-agreements
--     winget install --id Microsoft.msmpi   --accept-package-agreements
--   Linux:   any MPI-3 implementation with the one-sided calls the library
--     uses, e.g.
--     sudo apt install mpich libmpich-dev libhwloc-dev libfabric-dev
--     (the last two are pulled in by mpich.pc's Libs: -lhwloc -lfabric;
--      plain `mpicc` needs them too)  — or openmpi + libopenmpi-dev.
--
-- Detection lives in the gdi_mpi rule's on_load below and is overridable:
--   GDI_MPI_ROOT  Windows: SDK root with Include/mpi.h + Lib/<arch>/msmpi.lib
--                 Linux:   install prefix with include/mpi.h + lib/
--   MPI_HOME      Linux alias of GDI_MPI_ROOT
--   MSMPI / MSMPI_SDK / MSMPI_BIN  honoured as extra Windows candidates
--   (this-box fallbacks: "C:/Program Files (x86)/Microsoft SDKs/MPI" and
--    "C:/Program Files/Microsoft MPI/Bin")
--
-- Build:
--   xmake f -y -m release && xmake build gdi_rma   -> bin/release/gdi_rma.dll
--   xmake f -y -m debug   && xmake build gdi_rma   -> bin/debug/gdi_rma.dll
-- Optional smoke test (set_default(false), never part of a plain `xmake`):
--   xmake build smoke_gdi_rma
--   "C:/Program Files/Microsoft MPI/Bin/mpiexec.exe" -n 1 bin/release/smoke_gdi_rma
--
-- MSVC notes (why src/ carries tiny "win32 port (xmake build)" patches):
-- upstream built with `cc -O3 -Wall -Wextra -fcommon`; the -fcommon hid
-- tentative definitions of the predefined label/property-type globals in
-- gdi_label.h / gdi_property_type.h (now extern + defined once in
-- gdi_init.c) and a bare-inline function in gda_operation.h (now static
-- inline). With those fixed the sources link cleanly WITHOUT
-- /FORCE:MULTIPLE and without -fcommon, so neither flag is set anywhere.
--
-- Windows DLL exports: the public GDI_* API is exported through a generated
-- module definition file (build/gdi_rma.def, regenerated on every configure
-- from the prototypes in the public gdi*.h headers) passed to link.exe as
-- /DEF:. That keeps the sources free of __declspec() noise. The GDA_*
-- internal symbols are deliberately NOT exported; Phase 4 (src/api/*.c FFI
-- exports) is compiled INTO this same target and reaches them by direct
-- calls, exporting its own symbols with its own dllexport macro. If Phase 4
-- ever needs a GDA_* symbol outside the DLL, add its header to the
-- gdi_public_headers() list inside the gdi_mpi rule below.

set_xmakever("3.0.6")
add_rules("mode.release", "mode.debug", "mode.releasedbg")
set_defaultmode("release")
set_policy("check.auto_ignore_flags", false)

-- ----------------------------------------------------------------------------
-- gdi_mpi: attach the system MPI installation to a target.
--
-- The logic lives in the rule's on_load script field because that is the
-- scope where xmake's full io/os/os.execv sandbox API exists (at xmake.lua
-- project scope `io` and `os.execv` are not defined, and a main-chunk
-- closure would keep the poor main-chunk _ENV even when called from a
-- script field).  The rule is pure config: it only target:add()s fields.
-- ----------------------------------------------------------------------------
rule("gdi_mpi")
    on_load(function (target)

        -- headers that declare the public API surface of the DLL
        local function gdi_public_headers()
            return { "gdi.h", "gdi_constraint.h", "gdi_datatype.h",
                     "gdi_label.h", "gdi_property_type.h" }
        end

        -- MS-MPI SDK root (nil if none found)
        local function gdi_windows_mpi_sdk(arch)
            local candidates = {}
            for _, name in ipairs({ "GDI_MPI_ROOT", "MSMPI_SDK", "MSMPI" }) do
                local v = os.getenv(name)
                if v and v ~= "" then candidates[#candidates + 1] = v end
            end
            candidates[#candidates + 1] =
                "C:/Program Files (x86)/Microsoft SDKs/MPI"
            for _, root in ipairs(candidates) do
                if os.isfile(path.join(root, "Include", "mpi.h"))
                and os.isfile(path.join(root, "Lib", arch, "msmpi.lib")) then
                    return root, candidates
                end
            end
            return nil, candidates
        end

        -- directory holding msmpi.dll (runtime), nil if not found
        local function gdi_windows_mpi_bin()
            local candidates = {}
            local v = os.getenv("MSMPI_BIN")
            if v and v ~= "" then candidates[#candidates + 1] = v end
            for _, name in ipairs({ "GDI_MPI_ROOT", "MSMPI" }) do
                v = os.getenv(name)
                if v and v ~= "" then
                    candidates[#candidates + 1] = path.join(v, "Bin")
                end
            end
            candidates[#candidates + 1] = "C:/Program Files/Microsoft MPI/Bin"
            for _, dir in ipairs(candidates) do
                if os.isfile(path.join(dir, "msmpi.dll")) then return dir end
            end
            return nil
        end

        -- parse `pkg-config --cflags --libs <pkg>` output into target settings
        -- (xmake's script sandbox has no pcall; try/catch is its guard, and
        -- os.iorunv is the variant that returns captured stdout as a string)
        local function gdi_pkgconfig_mpi(pkg)
            local out = nil
            try { function() out = os.iorunv("pkg-config",
                                             { "--cflags", "--libs", pkg }) end }
            catch { function() out = nil end }
            if not out or out == "" then return false end
            -- token-wise parse: a regex on "-l" would also hit the "-l" inside
            -- e.g. /usr/lib/x86_64-linux-gnu/mpich, so split on whitespace
            local found = false
            for tok in out:gmatch("%S+") do
                if tok:startswith("-I") then
                    target:add("includedirs", tok:sub(3), {public = true})
                    found = true
                elseif tok:startswith("-L") then
                    target:add("linkdirs", tok:sub(3), {public = true})
                    target:add("rpathdirs", tok:sub(3))
                    found = true
                elseif tok:startswith("-l") then
                    target:add("links", tok:sub(3), {public = true})
                    found = true
                elseif tok:startswith("-W") or tok:startswith("-f")
                or tok:startswith("-pthread") then
                    -- extra hardening/optimization flags from the .pc file
                    target:add("shflags", tok)
                end
            end
            return found
        end

        local function gdi_setup_mpi()
            if target:is_plat("windows") then
                local sdk = gdi_windows_mpi_sdk(target:arch())
                if not sdk then
                    print("error: MS-MPI SDK not found. Set GDI_MPI_ROOT to")
                    print("       the SDK root (Include/mpi.h + Lib/"
                          .. target:arch() .. "/msmpi.lib) or install it:")
                    print("       winget install --id Microsoft.msmpisdk -y")
                    os.exit(1)
                end
                target:add("includedirs", path.join(sdk, "Include"),
                           {public = true})
                target:add("linkdirs", path.join(sdk, "Lib", target:arch()),
                           {public = true})
                target:add("links", "msmpi", {public = true})
            else
                local root = os.getenv("GDI_MPI_ROOT") or os.getenv("MPI_HOME")
                if root and root ~= "" and os.isdir(root) then
                    if os.isfile(path.join(root, "include", "mpi.h")) then
                        target:add("includedirs", path.join(root, "include"),
                                   {public = true})
                    end
                    if os.isdir(path.join(root, "lib")) then
                        target:add("linkdirs", path.join(root, "lib"),
                                   {public = true})
                        target:add("rpathdirs", path.join(root, "lib"))
                    end
                    target:add("links", "mpi", {public = true})
                    return
                end
                for _, pkg in ipairs({ "mpich", "mpich-v2", "openmpi" }) do
                    if gdi_pkgconfig_mpi(pkg) then return end
                end
                -- plain distro layout without pkg-config files (Debian/Ubuntu
                -- mpich: /usr/include/mpi.h + lib/x86_64-linux-gnu/libmpich.so)
                if os.isfile("/usr/include/mpi.h") then
                    for _, dir in ipairs({ "/usr/lib/x86_64-linux-gnu",
                                           "/usr/lib64", "/usr/lib",
                                           "/usr/local/lib",
                                           "/usr/local/lib64" }) do
                        for _, name in ipairs({ "mpich", "mpi" }) do
                            if os.isfile(path.join(dir, "lib" .. name .. ".so"))
                            or os.isfile(path.join(dir, "lib" .. name .. ".a"))
                            then
                                target:add("linkdirs", dir, {public = true})
                                target:add("rpathdirs", dir)
                                target:add("links", name, {public = true})
                                return
                            end
                        end
                    end
                end
                print("error: no MPI found (GDI_MPI_ROOT/MPI_HOME unset,")
                print("       pkg-config has no mpich/openmpi module,")
                print("       /usr/include/mpi.h absent). Install e.g.:")
                print("       sudo apt install mpich libmpich-dev")
                os.exit(1)
            end
        end

        -- Windows only: build/gdi_rma.def from the public header prototypes,
        -- handed to link.exe via /DEF:. Comment-stripped scan for
        -- `GDI_<name>(`; the gdi*.h headers contain no function-like macros
        -- and no function-pointer typedefs, so every match is an API
        -- prototype that one of src/*.c defines (verified 2026-10: 80 names,
        -- 80 definitions).
        local function gdi_write_windows_def()
            local names, seen = {}, {}
            for _, header in ipairs(gdi_public_headers()) do
                local file = path.join(os.projectdir(), "src", header)
                if os.isfile(file) then
                    local text = io.readfile(file)
                    text = text:gsub("/%*.-%*/", "")
                    text = text:gsub("%/%-[^\n]*", "")
                    for name in text:gmatch("(GDI_%a[%w_]*)%s*%(") do
                        if not seen[name] then
                            seen[name] = true
                            names[#names + 1] = name
                        end
                    end
                end
            end
            table.sort(names)
            local defpath = path.join(os.projectdir(),
                                      get_config("buildir") or "build",
                                      "gdi_rma.def")
            os.mkdir(path.directory(defpath))
            local f = io.open(defpath, "w")
            f:write("EXPORTS\n" .. table.concat(names, "\n") .. "\n")
            f:close()
            -- NOTE: xmake links a shared target with link.exe from SHFLAGS,
            -- ldflags are ignored on that path (verified on xmake v3.1.1:
            -- only add("shflags", ...) reaches the link.exe command line).
            target:add("shflags", "/DEF:" .. defpath)
        end

        -- ---- configuration for this target ----
        gdi_setup_mpi()
        if target:is_plat("windows") then
            target:add("defines", "_CRT_SECURE_NO_WARNINGS",
                       "WIN32_LEAN_AND_MEAN", "NOMINMAX", {public = true})
            if target:name() == "gdi_rma" then
                gdi_write_windows_def()
            end
        else
            if target:name() == "gdi_rma" then
                -- mirror the Makefile's -Wall -Wextra; -fPIC for the .so
                target:add("cxflags", "-Wall", "-Wextra", "-fPIC",
                           {force = true})
                -- typical MPI runtime deps (dlopen helpers, libm)
                target:add("syslinks", "pthread", "dl", "m")
            else
                -- smoke binary: find libgdi_rma.so sitting in bin/<mode>
                target:add("rpathdirs", target:targetdir())
            end
        end
    end)

    after_build(function (target)
        if target:is_plat("windows")
        and target:name() == "gdi_rma" then
            -- CONVENIENCE ONLY for local runs: copy msmpi.dll next to
            -- gdi_rma.dll. Not a staging step — the machine-wide install
            -- puts msmpi.dll in System32 and its Bin on PATH anyway.
            local bin = nil
            local candidates = {}
            local v = os.getenv("MSMPI_BIN")
            if v and v ~= "" then candidates[#candidates + 1] = v end
            for _, name in ipairs({ "GDI_MPI_ROOT", "MSMPI" }) do
                v = os.getenv(name)
                if v and v ~= "" then
                    candidates[#candidates + 1] = path.join(v, "Bin")
                end
            end
            candidates[#candidates + 1] = "C:/Program Files/Microsoft MPI/Bin"
            for _, dir in ipairs(candidates) do
                if os.isfile(path.join(dir, "msmpi.dll")) then
                    bin = dir
                    break
                end
            end
            if bin then
                os.cp(path.join(bin, "msmpi.dll"), target:targetdir())
            end
        end
    end)
rule_end()

-- ----------------------------------------------------------------------------
-- targets
-- ----------------------------------------------------------------------------

target("gdi_rma")
    set_kind("shared")
    set_basename("gdi_rma")
    add_rules("gdi_mpi")
    -- house convention: thirdparty/GDI-RMA/bin/{debug,release}/gdi_rma.dll
    set_targetdir("$(projectdir)/bin/$(mode)")
    -- src/Makefile OBJS == src/*.c (25 files); if upstream ever adds a
    -- main()-ful .c to src/, exclude it here rather than trusting the glob.
    add_files("src/*.c")
    add_headerfiles("src/*.h")
    add_includedirs("src")
    set_languages("c11")
    if is_mode("debug") then
        add_defines("DEBUG")
    else
        -- mirrors the Makefile's -O3: -O3 on gcc/clang, /O2 on msvc
        set_optimize("fastest")
    end
target_end()

-- optional end-to-end smoke test; NOT a default target (set_default(false)).
-- Build explicitly: xmake build smoke_gdi_rma
target("smoke_gdi_rma")
    set_kind("binary")
    set_default(false)
    add_rules("gdi_mpi")
    set_targetdir("$(projectdir)/bin/$(mode)")
    add_files("tests-smoke/gdi_smoke.c")
    add_includedirs("src")
    set_languages("c11")
    if is_mode("debug") then
        add_defines("DEBUG")
    end
    add_deps("gdi_rma")
target_end()
