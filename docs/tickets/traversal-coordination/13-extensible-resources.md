# 13 — Hold extensible ladders and force bridges safely

**Priority:** P1

**Difficulty:** medium

**What to build:** Let agents prepare and traverse extensible ladders and force bridges through physical controls and desired-state operations. Extension leases prevent either resource from retracting while an agent depends on it.

**Blocked by:**
- 05 — Prove typed interaction and device operations
- 12 — Coordinate opposing ladder and narrow-stair traffic

**Status:** complete

- [x] A retracted ladder or force bridge is conditionally routable only when an applicable control is reachable.
- [x] The selected operator requests an explicit extended state rather than toggling.
- [x] Equivalent extension requests share one operation while retaining individual ownership.
- [x] Occupancy, admission reservations, and active entry crossings each hold extension leases.
- [x] Retraction is rejected while any extension lease remains.
- [x] New admission stops after a safe retraction request is accepted.
- [x] An extensible ladder retains directional-capacity behaviour while extended.
- [x] Cancellation releases only the cancelling agent’s leases and interests.
- [x] Both resource types complete end-to-end agent journeys through the public headless seam.
