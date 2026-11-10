# The graphics release tier from the command line (S10, docs/ReleaseTier.md), on a copy of
# Reactor Run under the null renderer:
#   -DPLAYER=player -DPROJECT=Games/ShippingDemo -DTEMP=dir
# player --release-tier prints the tier and what each preset draws with, as text and JSON;
# a tier the presets can't meet, or one that doesn't read, exits 1 and says where;
# player --check --release reports it too; a project without the file has decided
# nothing and passes; pipeline_warmup is a project key that must be true or false.
foreach(var PLAYER PROJECT TEMP)
    if(NOT DEFINED ${var})
        message(FATAL_ERROR "ReleaseTierCheck.cmake needs -D${var}=...")
    endif()
endforeach()
file(REMOVE_RECURSE "${TEMP}")
set(G "${TEMP}/Reactor Run")
file(COPY "${PROJECT}/" DESTINATION "${G}" PATTERN "Saved" EXCLUDE)
set(headless SDL_VIDEO_DRIVER=offscreen SDL_AUDIO_DRIVER=dummy)
set(TIER "${G}/Config/ReleaseTier.swtier")

function(run expect)
    execute_process(COMMAND ${CMAKE_COMMAND} -E env ${headless} "${PLAYER}" --project "${G}" --renderer null ${ARGN}
        WORKING_DIRECTORY "${TEMP}" RESULT_VARIABLE code OUTPUT_VARIABLE out ERROR_VARIABLE err)
    if(NOT code EQUAL expect)
        string(REPLACE ";" " " shown "${ARGN}")
        message(FATAL_ERROR "${shown}: expected exit ${expect}, got ${code}:\n${out}\n${err}")
    endif()
    set(out_text "${out}${err}" PARENT_SCOPE)
endfunction()
function(expect_text pattern what)
    if(NOT out_text MATCHES "${pattern}")
        message(FATAL_ERROR "${what}: expected \"${pattern}\" in:\n${out_text}")
    endif()
endfunction()

# ---- Reactor Run's own tier ----
run(0 --release-tier)
expect_text("Config/ReleaseTier.swtier: fallback low; 1 mandatory, [0-9]+ optional" "the summary")
expect_text("shadow_quality +mandatory +low +low +low +medium +high +ultra" "the mandatory effect at every preset")
expect_text("bloom_quality +optional +- +low +low +medium +high +high" "an optional effect")
expect_text("mandatory shadow_quality: the sun's shadows" "the reason")
run(0 --release-tier --json "${TEMP}/tier.json")
file(READ "${TEMP}/tier.json" json)
string(JSON fallback GET "${json}" fallback)
string(JSON first_key GET "${json}" settings 0 key)
string(JSON first_use GET "${json}" settings 0 use)
string(JSON first_min GET "${json}" settings 0 minimum)
string(JSON problems LENGTH "${json}" diagnostics)
if(NOT fallback STREQUAL "low" OR NOT first_key STREQUAL "shadow_quality" OR NOT first_use STREQUAL "mandatory"
   OR NOT first_min STREQUAL "low" OR NOT problems EQUAL 0)
    message(FATAL_ERROR "--release-tier --json isn't the tier:\n${json}")
endif()
run(0 --check --release)
expect_text("ok Config/ReleaseTier.swtier" "--check --release names the tier")

# ---- A tier the fallback preset can't meet ----
file(WRITE "${TIER}" "swtier 1\nfallback low\nmandatory ambient_occlusion medium \"corners need grounding\"\n")
run(1 --release-tier)
expect_text("error Config/ReleaseTier.swtier:3: ambient_occlusion must be at least medium \\(corners need grounding\\), but the fallback tier low draws with off"
    "the fallback is too low")
run(1 --check --release)
expect_text("Config/ReleaseTier.swtier:3: ambient_occlusion must be at least medium" "--check --release fails on it")
run(2 --release-tier --check)   # one mode at a time

# ---- One that doesn't read ----
file(WRITE "${TIER}" "swtier 1\nfallback low\nmandatory shadow_quality best \"x\"\n")
run(1 --release-tier)
expect_text("Config/ReleaseTier.swtier:3: \"best\" isn't a value of shadow_quality \\(off, low, medium, high or ultra\\)"
    "a value that doesn't exist")
run(1 --graphics-preset fallback --check)
expect_text("--graphics-preset fallback needs a Config/ReleaseTier.swtier that reads" "fallback needs a tier")

# ---- No tier: nothing decided, nothing checked ----
file(REMOVE "${TIER}")
run(0 --release-tier)
expect_text("no Config/ReleaseTier.swtier: this project hasn't chosen a graphics release tier" "no tier")
run(0 --check --release)
if(out_text MATCHES "ReleaseTier")
    message(FATAL_ERROR "--check --release mentions a tier the project doesn't have:\n${out_text}")
endif()

# ---- The warm-up switch in the project file ----
file(GLOB project_file "${G}/*.swproject")
file(READ "${project_file}" project_text)
file(WRITE "${project_file}" "${project_text}\npipeline_warmup false\n")
run(0 --release-tier)
file(WRITE "${project_file}" "${project_text}\npipeline_warmup sometimes\n")
run(1 --release-tier)
expect_text("pipeline_warmup must be true or false" "a bad pipeline_warmup")
