# 14 — Complete a two-stop lift journey for one passenger

**Priority:** P1

**Difficulty:** hard

**What to build:** Deliver the first complete transport journey: one agent walks to a landing call control, summons a two-stop enclosed lift, boards through interlocked doors, confirms a destination, rides with the car, disembarks, and continues its path.

**Blocked by:**
- 07 — Operate a remote-controlled door through its button
- 09 — Enforce door safety and wide-threshold concurrency
- 11 — Enforce finite capacity on ladder transit

**Status:** ready-for-agent

- [ ] The path retains separate boarding, ride, and disembark edges but executes them as one transport journey.
- [ ] Registering trip intent alone does not dispatch the lift.
- [ ] Successfully using the landing control creates the pickup stop request.
- [ ] The landing and car doors open only while the correct lift car is aligned and stationary.
- [ ] Boarding reserves capacity and an interior standing position before crossing starts.
- [ ] Boarding commits the agent to the static lift transit and the lift-car manifest.
- [ ] Destination confirmation creates an onboard stop request before doors close.
- [ ] The passenger position remains local to the moving lift car and renders at the correct global position.
- [ ] The lift cannot move while a door or crossing interlock is active.
- [ ] Disembarking transfers the agent into the destination location and the path continues.
