# 20 — Migrate open platform lifts

**Priority:** P2

**Difficulty:** medium

**What to build:** Apply the transport journey model to an open platform lift. It uses a virtual boarding boundary instead of vehicle doors while preserving capacity, alignment, dwell, scheduling, passenger anchoring, and safe departure.

**Blocked by:** 17 — Recover lift journeys from failure and cancellation

**Status:** ready-for-agent

- [ ] The platform lift uses transport vehicle scheduling and manifests rather than a separate movement protocol.
- [ ] Boarding and disembarking are permitted only while the platform is aligned and stationary.
- [ ] A virtual boundary tracks active crossing permits without requiring a physical vehicle door.
- [ ] The platform cannot move while any boundary crossing remains active.
- [ ] Capacity is explicitly configured and validated against standing positions.
- [ ] Passengers remain spatially attached to the moving platform.
- [ ] Minimum dwell, boarding cutoff, destination confirmation, cancellation, and deactivation match enclosed-lift policy.
- [ ] A complete multi-stop platform journey passes through the public headless seam.
