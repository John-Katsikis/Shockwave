# Navigation: navmesh baking (Recast), path queries (Detour) and crowd steering
# (DetourCrowd) for NavAgent components during Play.
shockwave_module(Navigation PUBLIC_DEPS Shockwave::Engine PRIVATE_DEPS Recast Detour DetourCrowd)
