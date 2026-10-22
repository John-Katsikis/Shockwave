# The checked-in node reference is what the build generates.
execute_process(COMMAND "${PLAYER}" --project "${PROJECT}" --renderer null --node-schema -
    RESULT_VARIABLE result OUTPUT_VARIABLE generated ERROR_VARIABLE errors)
file(READ "${NODES}" checkedIn)
string(REPLACE "\r\n" "\n" checkedIn "${checkedIn}")
if(NOT result EQUAL 0 OR NOT generated STREQUAL checkedIn)
    message(FATAL_ERROR "docs/CircuitNodes.md is out of date; run: player --node-schema docs/CircuitNodes.md\n${errors}")
endif()
execute_process(COMMAND "${PLAYER}" --project "${PROJECT}" --renderer null
    --circuit Circuits/Spinner.swcircuit --run-circuit --circuit-ticks 1 --break-node spin
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE errors)
if(NOT result EQUAL 0 OR NOT output MATCHES "paused spin")
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
execute_process(COMMAND "${PLAYER}" --project "${PROJECT}" --renderer null
    --circuit Circuits/Door.swcircuit --run-circuit --send-message Open
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE errors)
if(NOT result EQUAL 0 OR NOT output MATCHES "node raise" OR output MATCHES "node lower")
    message(FATAL_ERROR "Door command failed (${result}):\n${output}\n${errors}")
endif()
execute_process(COMMAND "${PLAYER}" --project "${PROJECT}" --renderer null
    --circuit Circuits/Door.swcircuit --run-circuit --send-message Missing
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE errors)
if(result EQUAL 0 OR NOT errors MATCHES "No handler accepts")
    message(FATAL_ERROR "Missing command was not rejected: ${errors}")
endif()
