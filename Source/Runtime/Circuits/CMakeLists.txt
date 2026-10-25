# Circuits: visual scripting. .swcircuit node graphs in Content, compiled to bytecode
# and run by a small VM, attached to entities (the Circuit component) or to the whole
# game (game_circuit in the .swproject). See docs/Circuits.md.
shockwave_module(Circuits
    PUBLIC_DEPS Shockwave::Engine
    PRIVATE_DEPS Shockwave::Physics Shockwave::Audio Shockwave::GameUI Shockwave::Multiplayer)
