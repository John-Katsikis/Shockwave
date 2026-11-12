# Applies every *.patch in PATCH_DIR, in name order, to the Git checkout in the working
# directory. FetchContent runs this whenever it populates or updates the source; the
# checkout is put back to its pristine state first, so it always ends up as the release
# plus exactly these patches. A patch that doesn't apply stops the configure: a source
# update that conflicts with a patch can't build half patched.
find_package(Git REQUIRED)
set(ENV{GIT_LFS_SKIP_SMUDGE} 1)
execute_process(COMMAND ${GIT_EXECUTABLE} checkout -- . RESULT_VARIABLE result)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "Couldn't reset ${CMAKE_CURRENT_SOURCE_DIR} before patching")
endif()
file(GLOB patches ${PATCH_DIR}/*.patch)
list(SORT patches)
foreach(patch IN LISTS patches)
    get_filename_component(name ${patch} NAME)
    execute_process(COMMAND ${GIT_EXECUTABLE} apply --whitespace=nowarn ${patch} RESULT_VARIABLE result)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "${name} doesn't apply to this source; update the patch")
    endif()
    message(STATUS "Applied ${name}")
endforeach()
