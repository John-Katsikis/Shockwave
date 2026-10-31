# Particles (A04) on the GPU, with the graphics fixtures:
#   - soft particles: where puffs meet the floor, every blend mode (alpha, add, subtract,
#     multiply) fades to the scene without them; soft_particles false leaves the hard edge;
#     the near fade fades a puff by the camera;
#   - distortion: the haze bends the wall behind it, the post in front of it stays sharp
#     and doesn't bleed into it, and particle_distortion false draws none of it, with no
#     scene copy and no memory for one;
#   - scene lighting: lamps either side tint the lit smoke red and blue, which simple
#     lighting doesn't;
#   - the report's particles block and effective settings follow each setting.
cmake_policy(SET CMP0054 NEW)   # quoted strings in if() are never variables
file(MAKE_DIRECTORY "${TEMP}")
function(render scene name)
    execute_process(COMMAND "${PLAYER}" --project "${FIXTURE}/Graphics.swproject" --no-user-graphics
        --scene "${FIXTURE}/Scenes/${scene}.swscene" --fixed-step --size 640x360 --frames 5 ${ARGN}
        --screenshot "${TEMP}/${name}.png" RESULT_VARIABLE result OUTPUT_VARIABLE out ERROR_VARIABLE err)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "${scene} (${ARGN}) didn't render: ${out}\n${err}")
    endif()
endfunction()
function(report scene name)
    execute_process(COMMAND "${PLAYER}" --project "${FIXTURE}/Graphics.swproject" --no-user-graphics
        --scene "${FIXTURE}/Scenes/${scene}.swscene" --size 320x180 ${ARGN} --graphics-report --json "${TEMP}/${name}.json"
        RESULT_VARIABLE result OUTPUT_VARIABLE out ERROR_VARIABLE err)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "--graphics-report (${scene} ${ARGN}) failed: ${out}\n${err}")
    endif()
    file(READ "${TEMP}/${name}.json" json)
    set(json "${json}" PARENT_SCOPE)
endfunction()
# RMSE between two screenshots over a region.
function(rmse a b region out)
    execute_process(COMMAND "${PLAYER}" --image-diff "${TEMP}/${a}.png" "${TEMP}/${b}.png" --tolerance 255
        --region "${region}" --json "${TEMP}/diff.json" OUTPUT_QUIET)
    file(READ "${TEMP}/diff.json" diff)
    string(JSON value GET "${diff}" rmse)
    set(${out} "${value}" PARENT_SCOPE)
endfunction()
# Mean colour over a region, in whole levels.
function(mean image region out)
    execute_process(COMMAND "${PLAYER}" --image-stats "${TEMP}/${image}.png" --region "${region}"
        --json "${TEMP}/stats.json" OUTPUT_QUIET)
    file(READ "${TEMP}/stats.json" stats)
    string(JSON r GET "${stats}" mean_rgb 0)
    string(JSON g GET "${stats}" mean_rgb 1)
    string(JSON b GET "${stats}" mean_rgb 2)
    foreach(c r g b)   # whole levels: math() has no floats
        string(REGEX REPLACE "\\..*" "" ${c} "${${c}}")
    endforeach()
    set(${out} "${r};${g};${b}" PARENT_SCOPE)
endfunction()
# a < b * factor, for decimal numbers (math() has no floats): compared in thousandths.
function(less_than a b factor_permille out)
    foreach(v a b)
        string(REGEX MATCH "^[0-9]+" whole "${${v}}")
        string(REGEX MATCH "\\.([0-9]?[0-9]?[0-9]?)" frac "${${v}}")
        set(frac "${CMAKE_MATCH_1}000")
        string(SUBSTRING "${frac}" 0 3 frac)
        math(EXPR ${v}_m "${whole} * 1000 + 1${frac} - 1000")
    endforeach()
    math(EXPR limit "${b_m} * ${factor_permille} / 1000")
    if(a_m LESS limit)
        set(${out} TRUE PARENT_SCOPE)
    else()
        set(${out} FALSE PARENT_SCOPE)
    endif()
endfunction()

# ---- Soft particles ----
render(particles-soft-empty empty)
render(particles-soft soft-on --graphics-set soft_particles=true)
render(particles-soft soft-off --graphics-set soft_particles=false)
# Just above where each puff meets the floor (the floor is a few centimetres behind them
# there): alpha, add, subtract, multiply.
foreach(case "alpha;0.15" "add;0.36" "subtract;0.57" "multiply;0.77")
    list(GET case 0 mode)
    list(GET case 1 x)
    set(region "${x},0.629,0.07,0.004")
    rmse(soft-on empty "${region}" on)
    rmse(soft-off empty "${region}" off)
    message(STATUS "${mode} at the floor: soft ${on}, hard ${off} RMSE from no particles")
    less_than("${on}" "${off}" 300 ok)
    if(NOT ok OR off LESS 3)
        message(FATAL_ERROR "${mode} doesn't fade to the scene where it meets the floor: soft ${on}, hard ${off}")
    endif()
endforeach()
# The near puff, 1.3 m away, is a third of the way in (over 2 m from 0.5 m).
rmse(soft-on empty "0.815,0.29,0.06,0.05" near_on)
rmse(soft-off empty "0.815,0.29,0.06,0.05" near_off)
message(STATUS "near puff: soft ${near_on}, hard ${near_off}")
less_than("${near_on}" "${near_off}" 800 ok)
if(NOT ok OR near_off LESS 3)
    message(FATAL_ERROR "the near puff doesn't fade toward the camera: soft ${near_on}, hard ${near_off}")
endif()

# ---- Distortion ----
# Bloom off: it would spread the haze's changes onto the post.
render(particles-distortion haze-on --graphics-set particle_distortion=true --graphics-set bloom_quality=off)
render(particles-distortion haze-off --graphics-set particle_distortion=false --graphics-set bloom_quality=off)
rmse(haze-on haze-off "0.42,0.35,0.1,0.25" bent)
rmse(haze-on haze-off "0.575,0.45,0.02,0.4" post)
message(STATUS "distortion: wall behind the haze ${bent}, post in front ${post} RMSE from none")
if(bent LESS 5 OR NOT post EQUAL 0)
    message(FATAL_ERROR "distortion bent too little (${bent}) or touched the post in front of it (${post})")
endif()
# Right beside the post the haze shows the wall, never the post's red.
# (Without that, the bent look-ups pick up the post: about 20 levels more red than green here.)
mean(haze-on "0.552,0.40,0.014,0.2" beside)
list(GET beside 0 r)
list(GET beside 1 g)
math(EXPR red "${r} - ${g}")
if(red GREATER 8)
    message(FATAL_ERROR "the post bleeds into the haze beside it: mean ${beside}")
endif()

# ---- Scene lighting ----
render(particles-lit lit-scene --graphics-set particle_lighting=scene)
render(particles-lit lit-simple --graphics-set particle_lighting=simple)
foreach(case "red;0.40;0;2" "blue;0.55;2;0")
    list(GET case 0 lamp)
    list(GET case 1 x)
    list(GET case 2 hi)
    list(GET case 3 lo)
    mean(lit-scene "${x},0.4,0.05,0.15" scene)
    mean(lit-simple "${x},0.4,0.05,0.15" simple)
    list(GET scene ${hi} s_hi)
    list(GET scene ${lo} s_lo)
    list(GET simple ${hi} p_hi)
    list(GET simple ${lo} p_lo)
    math(EXPR tint_scene "${s_hi} - ${s_lo}")
    math(EXPR tint_simple "${p_hi} - ${p_lo}")
    message(STATUS "${lamp} side: scene lighting ${scene}, simple ${simple}")
    math(EXPR more "${tint_scene} - ${tint_simple}")
    if(more LESS 8)
        message(FATAL_ERROR "the ${lamp} lamp doesn't tint the smoke with scene lighting: ${scene} vs ${simple}")
    endif()
endforeach()

# ---- Reports ----
report(particles-soft soft-on --graphics-set soft_particles=true)
string(JSON soft_draws GET "${json}" particles soft_draws)
if(soft_draws EQUAL 0)
    message(FATAL_ERROR "soft_particles true faded nothing: ${json}")
endif()
report(particles-soft soft-off --graphics-set soft_particles=false)
string(JSON soft_draws GET "${json}" particles soft_draws)
if(NOT soft_draws EQUAL 0)
    message(FATAL_ERROR "soft_particles false still faded ${soft_draws} draws")
endif()
report(particles-distortion haze-on --graphics-set particle_distortion=true)
string(JSON graph GET "${json}" frame_graph)
string(JSON draws GET "${json}" particles distortion_draws)
string(JSON bytes GET "${json}" memory categories particles bytes)
if(NOT graph MATCHES "particle background copy" OR draws EQUAL 0)
    message(FATAL_ERROR "particle_distortion true drew no distortion: ${json}")
endif()
set(bytes_on ${bytes})
report(particles-distortion haze-off --graphics-set particle_distortion=false)
string(JSON graph GET "${json}" frame_graph)
string(JSON skipped GET "${json}" particles skipped_distortion)
string(JSON bytes GET "${json}" memory categories particles bytes)
if(graph MATCHES "particle background copy" OR skipped EQUAL 0 OR NOT bytes LESS bytes_on)
    message(FATAL_ERROR "particle_distortion false still copies the scene or keeps memory for it "
                        "(${bytes} vs ${bytes_on} bytes, ${skipped} skipped):\n${graph}")
endif()
report(particles-lit lit-simple --graphics-set particle_lighting=simple)
string(JSON lit GET "${json}" particles scene_lit_draws)
if(NOT lit EQUAL 0)
    message(FATAL_ERROR "particle_lighting simple still lit ${lit} draws by every light")
endif()
report(particles-lit lit-scene --graphics-set particle_lighting=scene)
string(JSON lit GET "${json}" particles scene_lit_draws)
if(lit EQUAL 0)
    message(FATAL_ERROR "particle_lighting scene lit nothing by every light: ${json}")
endif()
