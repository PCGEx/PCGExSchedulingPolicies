![UE 5.8](https://img.shields.io/badge/UE-5.8-darkgreen)

# PCGEx | Scheduling Policies

**Exotic runtime scheduling policies for PCG.**
Composable scheduling constraints (shapes, view signals, world targets), a 64-channel layering system so generation sources only trigger matching runtime components, and channel-aware graph nodes that switch content based on *who* triggered generation. Standalone -- no other PCGEx plugin required.

→ [Documentation](https://pcgex.gitbook.io/pcgex/essentials/tools/scheduling-policies)

## PCGEx Scheduling Policy

A composable replacement for the stock Distance & Direction policy -- pick it in the PCG component's `Scheduling Policy Class` and stack constraints:

- **Shapes** -- Sphere, Cylinder, Box (yaw-alignable), Cone; solid or **hollow** (surface shell with configurable thickness).
- **Source signals** -- Direction Alignment (priority), View Frustum (stock frustum culling parity).
- **World targets** -- Actor Bounds, Volumes (optional precise brush test), Splines (corridor radius, optional **closed-spline interior fill**); discovered via explicit references and/or actor-tag queries, with automatic rescans when targets move.
- `All`/`Any` combination, per-constraint invert and priority weight, cleanup hysteresis everywhere -- no boundary flicker.
- Blueprint-subclassable policy and constraints for reusable presets.

Constraints refine scheduling *within* the component/graph generation radii -- the engine broadphase always applies.

## Channels

Opt-in layering for runtime generation, defined once in **Project Settings → Plugins → PCGEx | Scheduling Policies**:

- Up to 64 named channels; each channel owns the actor tags that grant it (tag→channel mapping lives in the settings only -- single source of truth).
- **PCGEx Generation Source** component with a channel picker -- the preferred authoring path.
- Vanilla `PCG Generation Source` components participate through actor/component tags; automatic player sources get settings-assigned channels plus controller/pawn tags.
- The editor viewport source bypasses filtering by default so in-editor preview always works (per-policy toggle).

## Channel-aware graph nodes

Gate content by the channels active around the executing component -- decided at generation time, per grid cell:

- **Channel Gate** -- allow/block data (`Any`/`All`/`None` match, optional `Blocked` passthrough pin).
- **Channel Switch** -- route data to one output pin per channel (+ `Default`).
- **Get Active Channels** -- attribute set of the active channel names, for custom graph logic.

All three are cache-exempt (results depend on live world state) and fall back gracefully outside runtime generation.

## Roadmap

- Constraint debug draw
- More shapes (torus, …)
- Spline-emitting virtual generation sources
- Opt-in graph retrigger when the active channel set changes
- Blueprint-overridable constraint hooks
- Shared target snapshots across identical constraint specs
