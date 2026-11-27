# DXC without GVN: pass shares, DXIL size and a WARP check

[DXC compile time on a mixed-sampler compute shader](../experiments.md#dxc-compile-time-on-a-mixed-sampler-compute-shader)
found that Global Value Numbering (GVN) dominates DXC's time on the generated `samplebranch_N`
and `condstore_N` shaders. It also found that `-opt-disable gvn` removes most of that time, and
noted that the GVN-off DXIL had not been checked on a GPU. This note adds four things:

- GVN's pass share at the two larger `samplebranch_N` sizes;
- the DXIL size cost of switching GVN off;
- the sampler oracle on WARP, run on GVN-off DXIL;
- how long WARP takes to create and run those pipelines.

The shaders are the synthetic families of `tools/dxc_compile_time.py`, not translated title
shaders.

## Environment

Measured on 27.11.2026 on a Ryzen 9 7900X (12C/24T), Windows 11 build 26300, one compile at a time.

- **DXC:** 1.9.0.5512 (`01b62ad47`) from LunarG Vulkan SDK 1.4.363.0, the same build as the earlier tables.
- **WARP:** 10.0.26100.9818.
- **Oracle:** `offscreen_check` from a Debug `dev` build. The checkout was at `1ec2c938`; the binary was not rebuilt for this note. It printed `debug_layer=enabled`, and the layer reported 0 errors and 0 warnings on every run.
- **Repetitions:** every row is a single run. The earlier note found 20-45 % variation between sessions on the largest variants.

## GVN share of pass time

```powershell
out\pe-env\Scripts\python.exe -B tools\dxc_compile_time.py --dxc <path-to-dxc.exe> --variants samplebranch_4096 --ftime-report --timeout 90 --out out\dxc-compile-time\passes.json
out\pe-env\Scripts\python.exe -B tools\dxc_compile_time.py --dxc <path-to-dxc.exe> --variants samplebranch_8192,samplebranch_16384 --ftime-report --timeout 300 --out out\dxc-compile-time\passes-large.json
out\pe-env\Scripts\python.exe -B tools\dxc_compile_time.py --dxc <path-to-dxc.exe> --variants samplebranch_4096,samplebranch_8192,samplebranch_16384 --extra-arg=-opt-disable --extra-arg=gvn --ftime-report --timeout 300 --out out\dxc-compile-time\nogvn.json
```

| variant | HLSL bytes | wall s, default | pass s, default | GVN s (share) | other passes s | wall s, GVN off | pass s, GVN off |
|---|---:|---:|---:|---:|---:|---:|---:|
| samplebranch_4096 | 968,577 | 4.907 | 4.234 | 2.672 (63.1 %) | 1.56 | 2.328 | 1.609 |
| samplebranch_8192 | 1,946,457 | 17.821 | 16.453 | 12.688 (77.1 %) | 3.77 | 5.140 | 3.703 |
| samplebranch_16384 | 3,915,833 | 60.028 | 57.188 | 48.562 (84.9 %) | 8.63 | 11.194 | 8.125 |

- **GVN grows about fourfold per doubling** (4.75x, then 3.83x). The other passes grow 2.4x and 2.3x.
- **Switching GVN off leaves the rest unchanged:** the GVN-off pass time (1.61, 3.70, 8.13 s) matches the time of the other passes in the default runs. Removing GVN does not move work to another pass.
- **Speed-up:** 2.1x, 3.5x and 5.4x in wall time. `samplebranch_16384` compiles in 11.2 s here, against 18-28 s on the Ryzen 7 3700X in the earlier note. That earlier figure is still slower than `-Od`.
- **Largest passes with GVN off:** instruction combining (25.6-29.1 %) and the always-inline inliner (12.5-14.6 %).
- **Timer resolution:** the reported pass times are multiples of about 15.6 ms. Shares on variants below about 0.5 s are not meaningful.

## DXIL size

| variant | DXIL bytes, default | DXIL bytes, GVN off | change |
|---|---:|---:|---:|
| control | 4,020 | 4,024 | +0.1 % |
| samplebranch_4096 | 827,400 | 976,744 | +18.0 % |
| samplebranch_8192 | 1,802,276 | 2,071,784 | +15.0 % |
| samplebranch_16384 | 3,724,428 | 4,244,768 | +14.0 % |

A second compile with `--work-dir` gave the same sizes; the contents were not compared. Whether the
driver's compiler recovers what GVN would have removed, and what the larger DXIL costs at
pipeline creation or on the GPU, is not measured here.

## WARP check

```powershell
out\pe-env\Scripts\python.exe -B tools\dxc_compile_time.py --dxc <path-to-dxc.exe> --variants control,samplebranch_4096,samplebranch_8192 --extra-arg=-opt-disable --extra-arg=gvn --timeout 300 --work-dir out\dxc-oracle\nogvn --out out\dxc-oracle\nogvn.json
out\pe-env\Scripts\python.exe -B tools\dxc_compile_time.py --dxc <path-to-dxc.exe> --variants control,samplebranch_4096,samplebranch_8192 --timeout 300 --work-dir out\dxc-oracle\gvn --out out\dxc-oracle\gvn.json
out\build\dev\offscreen_check.exe warp --spirv-sampler-views out\dxc-oracle\nogvn\samplebranch_4096.cso
```

`offscreen_check --spirv-sampler-views` dispatches the shader under three sampler
arrangements and compares UAV words 0-3 with the fixture's expected values. Its UAV holds 16
bytes, so the generated body's stores fall outside it. The oracle shows that the GVN-off
DXIL creates a pipeline, dispatches, and keeps the control stores right. It does not check
the body that GVN would have optimized.

| CSO | result | wall s (pipeline creation and three dispatches) |
|---|---|---:|
| control, GVN off | pass | not timed |
| samplebranch_4096, default | pass | 79.2 |
| samplebranch_4096, GVN off | pass | 79.1 |
| samplebranch_8192, default | no result: blocked, stopped by hand | about 1,080 |
| samplebranch_8192, GVN off | not run | - |
| negative control | fails as expected | not timed |

- **Passing runs:** every one printed `sample 0.25 0.75, comparison 0 1`, then `0.75 0.25, 0 1`, then `0.25 0.75, 1 0`.
- **Negative control:** in the GVN-off `samplebranch_4096` HLSL, the u coordinate of the first control store was changed from 1.25 to 0.25, and the file was compiled with the same flags. With a clamping sampler the shader then reads texel 0 instead of texel 1. The oracle exited with status 1 and reported word 1 of swaps 0 and 2 and word 0 of swap 1 as `expected=3f400000 actual=3e800000` (0.75 against 0.25).
- **Same WARP time with or without GVN:** at `samplebranch_4096`, the 18 % larger GVN-off DXIL took the same time on WARP. WARP's time, 79 s, is 16 times DXC's time with GVN on and 34 times with GVN off.
- **`samplebranch_8192` blocked on WARP:** the default-optimization CSO used 27.4 s of CPU time. After that, sampled 17 minutes after the start, the process had one thread, the same CPU time in two samples 3 s apart, and a steady 228 MiB working set. It was stopped after about 18 minutes, so it did not reach a readback. Where it waits was not determined. The GVN-off 8192 CSO was not run.
- **Earlier runs:** the earlier note ran WARP only up to `samplebranch_4096`, with default optimization. On an RTX 4060, `samplebranch_12288` pipeline creation did not return within about 9 minutes.

## Not covered

- Any adapter other than WARP, and WARP beyond `samplebranch_4096`.
- How WARP's 79 s splits between pipeline creation and the three dispatches.
- The body stores of the generated shaders, and any translated title shader.
- Whether `-opt-disable gvn` is safe as a translation setting. This note does not propose it as one.
- Why GVN scales this way. The earlier note's mechanism, repeated handle creation feeding non-local memory-dependence queries, remains a hypothesis.
