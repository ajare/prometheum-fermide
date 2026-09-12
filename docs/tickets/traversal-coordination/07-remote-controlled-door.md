# 07 — Operate a remote-controlled door through its button

**Priority:** P0

**Difficulty:** hard

**What to build:** Complete a journey through a remote-controlled door by assigning an agent to a reachable button, issuing typed open commands, waiting for their outcome, and then crossing. Several waiting agents share one active preparation request rather than all pressing the button.

**Blocked by:** 06 — Traverse manual and automatic doors safely

**Status:** complete

- [x] A remote-controlled door cannot be opened directly by traversal execution.
- [x] The traversal resource selects the earliest eligible waiting agent able to reach an applicable control.
- [x] The selected operator reserves the control position and retains logical traversal priority.
- [x] Other agents reuse the active open request without pressing the button.
- [x] If the door becomes open while the operator approaches, the redundant interaction is cancelled cleanly.
- [x] Temporary blocking retries deterministically; bounded execution failure and permanent rejection produce distinct outcomes.
- [x] A multi-command control succeeds only when every required binding succeeds.
- [x] Cancelling one waiting agent does not cancel an opening operation still needed by others.
- [x] If no applicable control is reachable, the edge is reported unavailable for execution and replanning.
