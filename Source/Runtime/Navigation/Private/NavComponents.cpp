#include <Navigation/NavComponents.h>

#include <Engine/Components.h>

namespace shockwave {

void registerNavigationComponents() {
    registerComponent<NavMeshSettings>(
        "NavMeshSettings",
        {
            enumField("geometry", &NavMeshSettings::geometry, {"Static colliders", "Render meshes", "Both"}),
            field("agent_radius", &NavMeshSettings::agentRadius, 0.01f, 0.05f, 10.0f),
            field("agent_height", &NavMeshSettings::agentHeight, 0.01f, 0.1f, 20.0f),
            field("max_climb", &NavMeshSettings::maxClimb, 0.01f, 0.0f, 10.0f),
            field("max_slope", &NavMeshSettings::maxSlope, 0.5f, 0.0f, 89.0f),
            field("cell_size", &NavMeshSettings::cellSize, 0.005f, 0.02f, 2.0f),
            field("cell_height", &NavMeshSettings::cellHeight, 0.005f, 0.02f, 2.0f),
            field("tile_size", &NavMeshSettings::tileSize, 1.0f, 16.0f, 256.0f),
            field("auto_bounds", &NavMeshSettings::autoBounds),
            field("bounds_min", &NavMeshSettings::boundsMin, 0.1f),
            field("bounds_max", &NavMeshSettings::boundsMax, 0.1f),
            field("max_agents", &NavMeshSettings::maxAgents, 1.0f, 1.0f, 4096.0f),
        },
        "Navigation");
    registerComponent<NavAgent>(
        "NavAgent",
        {
            field("speed", &NavAgent::speed, 0.05f, 0.0f, 100.0f),
            field("acceleration", &NavAgent::acceleration, 0.1f, 0.0f, 1000.0f),
            field("radius", &NavAgent::radius, 0.01f, 0.05f, 10.0f),
            field("height", &NavAgent::height, 0.01f, 0.1f, 20.0f),
            field("stopping_distance", &NavAgent::stoppingDistance, 0.01f, 0.0f, 100.0f),
            field("target", &NavAgent::target),
            field("destination", &NavAgent::destination, 0.05f),
            field("has_destination", &NavAgent::hasDestination),
            enumField("avoidance", &NavAgent::avoidance, {"None", "Low", "Medium", "High"}),
            field("separation", &NavAgent::separation, 0.01f, 0.0f, 20.0f),
            field("face_movement", &NavAgent::faceMovement),
            field("enabled", &NavAgent::enabled),
        },
        "Navigation");
    registerComponent<NavModifier>(
        "NavModifier", {enumField("mode", &NavModifier::mode, {"Walkable", "Not walkable", "Ignore"})}, "Navigation");
}

}   // namespace shockwave
