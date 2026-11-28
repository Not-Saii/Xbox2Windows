# Named reasons for a compute output buffer range

A translated compute program's buffer output is bound only when a placed buffer aliases the title memory it
names, so the GPU writes reach the title. The single refusal `an output is not an aliased placed buffer range
with unordered access` is split into the condition that failed, in the same log and census lines, checked in
this order:

1. `an output buffer descriptor is a view token`
2. `an output buffer descriptor has zero records`
3. `an output buffer range is over 16 MiB or not whole dwords`
4. `an output range crosses the end of a placed buffer` (also a range that two placements claim)
5. `an output is in title memory outside any placed buffer`
6. `an output is in a placed buffer that does not alias title memory` (a copied placement)
7. `an output buffer's resource was not created in the common state` (also an aliased resource with no recorded
   information, which no fixture reaches)
8. `an output buffer range is not 16-byte aligned in its resource`
9. `an output buffer has no unordered access`

Only the reason changes: what was bound is still bound and what was refused is still refused. Output view tokens
are lowered separately in #2961 (open), which binds a buffer UAV token before these checks; with it, reason 1 is
not reached. The census already groups refused dispatches by each key's first reason, so a start-up run with this
build gives the count of every condition.

`tests/startup_compute_dispatch_output_check.py` dispatches case A of `startup_gfx2hlsl_compute_check.py` through
the stand-in's list on WARP and on a physical adapter (exit 77 without one). The unchanged recording matches all
320 words. Each of the nine conditions is then produced by one change to the output descriptor or its buffer,
and each dispatch refuses with its own reason in the log and the census and leaves every output word at its
sentinel. The common-state case also checks that its buffer was aliased. `dispatch-swapped` in
`startup_compute_dispatch_check.py` now expects reason 9.

```powershell
ctest --preset dev --no-tests=error -R "^startup_compute_dispatch(_output)?_(warp|hardware)$" -V
```

Both tests pass on WARP (10.0.26100.9818) and on an RTX 4090 (driver 32.0.16.1088), Windows 11 build 26300,
debug layer on. With only the split reverted, the unchanged dispatch still matches and all nine cases are still
refused, each with the old reason, so all nine expectations fail: the set of refused cases is the same.
