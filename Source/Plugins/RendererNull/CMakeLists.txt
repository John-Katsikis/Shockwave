# The headless renderer plugin: bin/plugins/renderer_null.so. Draws nothing, needs no
# GPU: for servers, tests and tools that only run game logic.
shockwave_plugin(renderer null DEPS Shockwave::Renderer)
