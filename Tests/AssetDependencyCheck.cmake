# Asset dependencies and residency on the command line (S04, docs/AssetResidency.md), on a
# copy of Tests/fixtures/levels with a material and a texture added to level b:
#   - --asset-dependencies lists each level's closure as text and JSON, --level narrows it;
#   - --check and --asset-dependencies fail on a missing file and on a material cycle, with
#     their codes, and pass again once they're fixed;
#   - --load-report writes the start's phases and assets, and nothing stays resident;
#   - --no-asset-residency turns leases off; a budget too small fails with BudgetExceeded.
#   -DPLAYER=player -DFIXTURE=Tests/fixtures/levels -DTEXTURE=a .png -DTEMP=dir
cmake_policy(SET CMP0054 NEW)
file(REMOVE_RECURSE "${TEMP}")
file(MAKE_DIRECTORY "${TEMP}")
file(COPY "${FIXTURE}/" DESTINATION "${TEMP}/Levels")
set(root "${TEMP}/Levels")
set(project "${root}/Levels.swproject")
file(COPY "${TEXTURE}" DESTINATION "${root}/Content/textures")
get_filename_component(texture "${TEXTURE}" NAME)
file(WRITE "${root}/Content/materials/sand.swmaterial" "swmaterial 1\ntexture base_color \"textures/${texture}\"\n")
file(READ "${root}/Scenes/b.swscene" sceneB)
string(REPLACE "    mesh floor\n" "    mesh floor\n    material \"materials/sand.swmaterial\"\n" sceneB "${sceneB}")
file(WRITE "${root}/Scenes/b.swscene" "${sceneB}")

function(run expected pattern)
    execute_process(COMMAND "${PLAYER}" --project "${project}" ${ARGN}
                    RESULT_VARIABLE result OUTPUT_VARIABLE out ERROR_VARIABLE err)
    if(NOT result EQUAL ${expected} OR NOT "${out}${err}" MATCHES "${pattern}")
        message(FATAL_ERROR "player ${ARGN}: expected exit ${expected} and '${pattern}', got ${result}:\n${out}\n${err}")
    endif()
    set(out "${out}${err}" PARENT_SCOPE)
endfunction()

# ---- The good fixture ----
run(0 "level b: 3 assets.*material Content/materials/sand.swmaterial.*texture Content/textures/${texture}.*ok"
    --asset-dependencies --renderer null)
run(0 "ok asset dependencies" --check --renderer null)
run(0 "Wrote" --asset-dependencies --renderer null --json "${TEMP}/deps.json")
file(READ "${TEMP}/deps.json" json)
string(JSON format GET "${json}" format)
string(JSON mode GET "${json}" mode)
string(JSON ok GET "${json}" ok)
string(JSON roots LENGTH "${json}" roots)
string(JSON bName GET "${json}" roots 2 name)
string(JSON bAssets LENGTH "${json}" roots 2 assets)
string(JSON bHash GET "${json}" roots 2 closure_hash)
if(NOT format EQUAL 1 OR NOT mode STREQUAL "asset_dependencies" OR NOT ok STREQUAL "ON" OR NOT roots EQUAL 3
   OR NOT bName STREQUAL "b" OR NOT bAssets EQUAL 4 OR NOT bHash MATCHES "^[0-9a-f]+$")
    message(FATAL_ERROR "--asset-dependencies --json: ${json}")
endif()
run(0 "Wrote" --asset-dependencies --level menu --renderer null --json "${TEMP}/menu.json")
file(READ "${TEMP}/menu.json" json)
string(JSON roots LENGTH "${json}" roots)
string(JSON menuAsset GET "${json}" roots 0 assets 0)
if(NOT roots EQUAL 1 OR NOT menuAsset STREQUAL "Content/UI/menu.rml")
    message(FATAL_ERROR "--asset-dependencies --level menu: ${json}")
endif()
run(1 "no such level" --asset-dependencies --level nowhere --renderer null)

# ---- A changed texture changes b's closure hash, not a's ----
run(0 "Wrote" --asset-dependencies --renderer null --json "${TEMP}/before.json")
file(READ "${TEMP}/before.json" before)
file(APPEND "${root}/Content/textures/${texture}" "changed")
run(0 "Wrote" --asset-dependencies --renderer null --json "${TEMP}/after.json")
file(READ "${TEMP}/after.json" after)
string(JSON aBefore GET "${before}" roots 1 closure_hash)
string(JSON aAfter GET "${after}" roots 1 closure_hash)
string(JSON bBefore GET "${before}" roots 2 closure_hash)
string(JSON bAfter GET "${after}" roots 2 closure_hash)
if(NOT aBefore STREQUAL aAfter OR bBefore STREQUAL bAfter)
    message(FATAL_ERROR "closure hashes: a ${aBefore} -> ${aAfter}, b ${bBefore} -> ${bAfter}")
endif()

# ---- Mistakes: a missing texture, then a cycle ----
file(WRITE "${root}/Content/materials/sand.swmaterial" "swmaterial 1\ntexture base_color \"textures/gone.png\"\n")
run(1 "AssetMissing" --check --renderer null)
run(1 "error AssetMissing: Content/materials/sand.swmaterial: .*textures/gone.png" --asset-dependencies --renderer null)
file(WRITE "${root}/Content/materials/sand.swmaterial" "swmaterial 1\nparent \"materials/dune.swmaterial\"\n")
file(WRITE "${root}/Content/materials/dune.swmaterial" "swmaterial 1\nparent \"materials/sand.swmaterial\"\n")
run(1 "AssetCycle" --check --renderer null)
run(1 "\"code\": \"AssetCycle\"" --asset-dependencies --renderer null --json -)
file(WRITE "${root}/Content/materials/sand.swmaterial" "swmaterial 1\ntexture base_color \"textures/${texture}\"\n")
file(REMOVE "${root}/Content/materials/dune.swmaterial")
run(0 "ok asset dependencies" --check --renderer null)

# ---- --load-report, --no-asset-residency, budgets ----
run(0 "Wrote" --renderer null --frames 3 --user-dir "${TEMP}/User" --load-report "${TEMP}/load.json")
file(READ "${TEMP}/load.json" load)
string(JSON mode GET "${load}" mode)
string(JSON residency GET "${load}" residency)
string(JSON level GET "${load}" loads 0 level)
string(JSON status GET "${load}" loads 0 status)
string(JSON assets GET "${load}" loads 0 assets)
string(JSON start GET "${load}" loads 0 phases_ms start)
string(JSON resident GET "${load}" resident assets)
string(JSON leases GET "${load}" resident leases)
if(NOT mode STREQUAL "load_report" OR NOT residency STREQUAL "ON" OR NOT level STREQUAL "menu"
   OR NOT status STREQUAL "succeeded" OR NOT assets EQUAL 2 OR start LESS 0 OR NOT resident EQUAL 0
   OR NOT leases EQUAL 0)
    message(FATAL_ERROR "--load-report: ${load}")
endif()
run(0 "Wrote" --renderer null --frames 3 --user-dir "${TEMP}/User" --no-asset-residency --load-report "${TEMP}/off.json")
file(READ "${TEMP}/off.json" off)
string(JSON residency GET "${off}" residency)
string(JSON assets GET "${off}" loads 0 assets)
if(NOT residency STREQUAL "OFF" OR NOT assets EQUAL 0)
    message(FATAL_ERROR "--no-asset-residency: ${off}")
endif()
run(1 "BudgetExceeded: CPU" --renderer null --frames 3 --user-dir "${TEMP}/User" --asset-budget-cpu 0.0001)
run(0 "" --renderer null --frames 3 --user-dir "${TEMP}/User" --asset-budget-cpu 64 --asset-budget-gpu 64)
run(2 "takes a size in MiB" --renderer null --asset-budget-gpu lots)
message(STATUS "asset dependencies command line: all checks passed")
