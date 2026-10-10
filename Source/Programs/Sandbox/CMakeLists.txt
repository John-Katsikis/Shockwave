# Sandbox: a tiny game that uses the engine, for trying features as they land.
add_executable(sandbox Private/Main.cpp)
target_link_libraries(sandbox PRIVATE Shockwave::Engine)
shockwave_warnings(sandbox)
