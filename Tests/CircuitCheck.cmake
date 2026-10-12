execute_process(COMMAND "${PLAYER}" --project "${PROJECT}" --renderer null
    --circuit Circuits/Spinner.swcircuit --run-circuit --circuit-ticks 1 --break-node spin
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE errors)
if(NOT result EQUAL 0 OR NOT output MATCHES "paused spin" OR NOT output MATCHES "node turned")
    message(FATAL_ERROR "Circuit runner breakpoint failed (${result}):\n${output}\n${errors}")
endif()
execute_process(COMMAND "${PLAYER}" --project "${PROJECT}" --renderer null
    --circuit Circuits/Spinner.swcircuit --run-circuit --break-node spin --circuit-steps 1
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE errors)
if(NOT result EQUAL 0 OR NOT output MATCHES "node spin" OR output MATCHES "paused spin")
    message(FATAL_ERROR "Circuit runner stepping failed (${result}):\n${output}\n${errors}")
endif()
execute_process(COMMAND "${PLAYER}" --project "${PROJECT}" --renderer null
    --circuit Circuits/Spinner.swcircuit --run-circuit --break-node nonexistent
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE errors)
if(result EQUAL 0 OR NOT errors MATCHES "Unknown breakpoint node")
    message(FATAL_ERROR "Unknown breakpoint was not rejected: ${errors}")
endif()
