# Recast & Detour: navigation mesh baking (Recast), path queries (Detour) and crowd
# steering (DetourCrowd) for the Navigation module. Only the libraries are built: no
# demo, tests or examples. They stay static and private to ShockwaveNavigation.
set(RECASTNAVIGATION_DEMO OFF CACHE BOOL "" FORCE)
set(RECASTNAVIGATION_TESTS OFF CACHE BOOL "" FORCE)
set(RECASTNAVIGATION_EXAMPLES OFF CACHE BOOL "" FORCE)
FetchContent_Declare(recastnavigation
    GIT_REPOSITORY https://github.com/recastnavigation/recastnavigation.git
    GIT_TAG v1.6.0
    GIT_SHALLOW TRUE
)
# Its CMakeLists asks for CMake 3.1, which CMake 4 refuses unless told the 3.5 rules are fine.
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
FetchContent_MakeAvailable(recastnavigation)
unset(CMAKE_POLICY_VERSION_MINIMUM)
# Its zlib license ships with the binaries, next to the other libraries' (build/bin/licenses).
configure_file(${recastnavigation_SOURCE_DIR}/License.txt ${CMAKE_BINARY_DIR}/bin/licenses/RecastNavigation-License.txt
               COPYONLY)
