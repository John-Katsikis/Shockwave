# Platform: the OS window and events, through SDL3.
shockwave_module(Platform PUBLIC_DEPS Shockwave::Core SDL3::SDL3)
if(WIN32)
    target_link_libraries(ShockwavePlatform PRIVATE psapi)   # GetProcessMemoryInfo (SystemInfo.cpp)
endif()
