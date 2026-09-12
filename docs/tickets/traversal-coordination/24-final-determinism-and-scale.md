# 24 — Verify determinism, scale, and complete behaviour

**Priority:** P2

**Difficulty:** hard

**What to build:** Run the completed coordination model as one integrated simulation and prove that it remains deterministic, observable, and responsive at the target scale. Close any externally visible gaps without reintroducing cross-cutting ownership or controller abstractions.

**Blocked by:** 23 — Contract and remove the legacy controller hierarchy

**Status:** ready-for-agent

- [ ] The full required scenario suite passes through the public headless simulation seam.
- [ ] Repeated runs with identical worlds and intents produce identical snapshots and event order.
- [ ] Scenarios cover ordinary and controlled doors, cancellation, expiry, lift and shuttle capacity, LOOK scheduling, disembark priority, destination confirmation, ladder contention, force bridges, windows, bulkheads, and platform lifts.
- [ ] Player-directed agents obey the same traversal rules as autonomous agents.
- [ ] Read-only diagnostics expose enough state to explain every waiting, rejected, failed, and active traversal.
- [ ] A representative world with hundreds of active agents and dozens of resources remains responsive without global collision checks or global per-tick replanning.
- [ ] A stretch scenario with approximately 1,000 agents records timing and memory observations for future optimization.
- [ ] No resource exceeds capacity or lane count, and no completed cancellation leaves owned state behind.
- [ ] Debug and Release application and headless builds pass.
- [ ] The implementation and domain documentation reflect the final observable behaviour.
