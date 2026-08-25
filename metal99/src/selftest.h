/*
 * On-device verification. No human required.
 *
 * The panel cannot be read back, so correctness used to depend on someone
 * describing the display - a loop that is slow and, worse, cannot distinguish a
 * wedged panel showing stale content from wrong pixels being drawn. Several
 * wrong conclusions came from exactly that ambiguity.
 *
 * These check what the hardware was ACTUALLY told to send, and are themselves
 * validated against injected faults, on-device and in tests/host.
 *
 * A verifier that always passes is worthless, and this project has shipped
 * that verifier repeatedly - five digests (see fold.c) plus a
 * selftest_transport() whose return value was a shadowed variable the compiler
 * folded to a constant zero. Assume the harness is broken until it has
 * rejected something.
 */
#ifndef SELFTEST_H
#define SELFTEST_H

/*
 * PROBE PATTERN RAMP - defined HERE so there is exactly one copy.
 *
 * selftest.c builds the pattern with these; tests/host/digest_test.c asserts
 * the pattern's properties with these. They were briefly duplicated in both
 * files, kept in step by a comment saying "must match" - which is the same
 * drift-by-two-copies that put "q4-q7 UNUSED" in vec.h and CONTRIBUTING.md
 * long after q4-q7 were taken.
 *
 * vec_ramp16 accumulates with a SIGNED SATURATING add, so START + (W-1)*STEP
 * must stay inside int16 or the ramp goes flat. selftest.c static-asserts it.
 */
#define PROBE_START 0x1234
#define PROBE_STEP  0x004B

/* Fill white and pulse brightness. Answers "is the panel alive?" using only
 * the command path, with no elision, banding or DMA involved. A dark screen is
 * otherwise ambiguous between a wedged panel and code correctly drawing black. */
void selftest_liveness(void);

/* Compare transmitted bytes against an independently computed digest, on both
 * transports, then prove the digest REJECTS known faults.
 * Returns the number of failures; 0 is a pass. */
int  selftest_transport(void);

#endif /* SELFTEST_H */
