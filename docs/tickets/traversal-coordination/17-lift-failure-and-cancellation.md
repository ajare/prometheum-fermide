# 17 — Recover lift journeys from failure and cancellation

**Priority:** P1

**Difficulty:** hard

**What to build:** Make lift travel safe and recoverable when interactions fail, routes change, calls arrive after cutoff, or the lift becomes unavailable. Every request and reservation reaches a deterministic terminal outcome without stranding passengers or leaking ownership.

**Blocked by:** 16 — Schedule multi-stop lifts with LOOK

**Status:** complete

- [x] A failed destination interaction retries within policy limits while doors remain safely open.
- [x] If destination confirmation cannot succeed, the passenger leaves at the current stop and its journey fails cleanly.
- [x] A late boarding call after cutoff remains queued for later service and does not reopen closing doors.
- [x] An obstruction or new safety lease does reopen transport doors.
- [x] Cancelling a route while moving retains occupancy until the next safe stop.
- [x] A compatible replacement onboard route can submit a reachable new destination without forcing an exit.
- [x] Disabling a moving lift rejects new requests, reaches the next safe stop, unloads safely, and then becomes unavailable.
- [x] Remaining waiting requests receive failure reasons and can replan.
- [x] Invalid capacity, standing positions, stop geometry, controls, and landing/car-door mappings fail construction with precise diagnostics.
- [x] No failure path leaves stale queue tickets, capacity reservations, stop-request owners, or open leases.
