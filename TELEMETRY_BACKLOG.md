# Telemetry backlog

## Deferred: batched shot telemetry

Per-shot telemetry is intentionally not part of schema 22. This branch adds no
shot marker, consumer handler, database contract, or runtime collection path.

Reconsider it only after real schema-22 matches demonstrate sustained headroom
at the two-second position cadence with zero producer drops. A future design
must be minimal and batched, define exactly what a “shot” means for every DoD
weapon class, preserve the existing documented bot/pause coverage gaps, and pass
a storage/EPS soak before any production rollout. Until then, aggregate
weaponstats remain the authoritative accuracy source.
