# 14 — Complete a two-stop lift journey for one passenger

**Priority:** P1

**Difficulty:** hard

**What to build:** Deliver the first complete transport journey: one agent walks to a landing call control, summons a two-stop enclosed lift, boards through interlocked doors, confirms a destination, rides with the car, disembarks, and continues its path.

**Blocked by:**
- 07 — Operate a remote-controlled door through its button
- 09 — Enforce door safety and wide-threshold concurrency
- 11 — Enforce finite capacity on ladder transit

**Status:** complete

- [x] The path retains separate boarding, ride, and disembark edges but executes them as one transport journey.
- [x] Registering trip intent alone does not dispatch the lift.
- [x] Successfully using the landing control creates the pickup stop request.
- [x] The landing and car doors open only while the correct lift car is aligned and stationary.
- [x] Boarding reserves capacity and an interior standing position before crossing starts.
- [x] Boarding commits the agent to the static lift transit and the lift-car manifest.
- [x] Destination confirmation creates an onboard stop request before doors close.
- [x] The passenger position remains local to the moving lift car and renders at the correct global position.
- [x] The lift cannot move while a door or crossing interlock is active.
- [x] Disembarking transfers the agent into the destination location and the path continues.
