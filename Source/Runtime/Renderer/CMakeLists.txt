# Renderer: the renderer games and tools use (meshes, textures, lights, the frame). The
# drawing is done by a renderer plugin (Source/Plugins/Renderer*), which it can swap or
# reload while running.
shockwave_module(Renderer PUBLIC_DEPS Shockwave::Core Shockwave::Platform)
