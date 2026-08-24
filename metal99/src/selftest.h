/*
 * On-device verification. No human required.
 *
 * The panel cannot be read back, so correctness used to depend on someone
 * describing the display - a loop that is slow and, worse, cannot distinguish a
 * wedged panel showing stale content from wrong pixels being drawn. Several
 * wrong conclusions came from exactly that ambiguity.
 *
 * These check what the hardware was ACTUALLY told to send, and are themselves
 * validated against injected faults. A verifier that always passes is
 * worthless; three earlier versions of this were exactly that.
 */
#ifndef SELFTEST_H
#define SELFTEST_H

/* Fill white and pulse brightness. Answers "is the panel alive?" using only
 * the command path, with no elision, banding or DMA involved. A dark screen is
 * otherwise ambiguous between a wedged panel and code correctly drawing black. */
void selftest_liveness(void);

/* Compare transmitted bytes against an independently computed digest, on both
 * transports, then prove the digest REJECTS known faults.
 * Returns the number of failures; 0 is a pass. */
int  selftest_transport(void);

#endif /* SELFTEST_H */
