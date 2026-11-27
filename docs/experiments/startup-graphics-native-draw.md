# In-process VS/PS compile on the first draw in the stand-in (#2978)

[The fixture](../../tests/startup_draw_native_check.py) creates a source-built gfx1013 VS/PS pair through the
stand-in's device slot 86 and draws it with `X2W_NATIVE_SHADERS` set, an empty shader cache, no offline
translator and no prebuilt stage bytecode. It compares a pixel inside and a pixel outside the viewport with the
shader's colour and the clear colour, then repeats the draw in a second process on the populated cache.

## Route

1. Pipeline creation publishes `req_<key>.{b,root,a}` as before (`cached_translation` in
   `src/startup_stubs/d3d12_draw.inl`) and finds no `<key>.txt`. Nothing is compiled there.
2. On the first draw of the pipeline, `compile_pair_in_process` runs `x2w::compile_graphics_request`
   (`src/graphics_request_job.cpp`) on the request. That function is the graphics job
   `compile_dxil gfx1013-container-pair --requests` runs, moved out of the tool so both call the same code:
   `read_graphics_request` checks the parts against the key in the file name, `prepare_direct_graphics` links the
   pair, `format_graphics_runtime_manifest` writes the bindings, and `ShaderDxilCompiler::runtime_worker`
   compiles each stage.
3. It publishes `<key>.vs.cso`, `<key>.ps.cso` and then `<key>.txt`. `load_translation` reads them as it reads
   any published entry, and `pipeline_for` creates the desktop pipeline. The frontend's `.pairir` and the
   compiler's DXIL cache go to `<shader cache>\native`.
4. A later process finds `<key>.txt` at pipeline creation and compiles nothing. Its first draw logs
   `cache entry loaded, no in-process compile`.

In the stand-in a pair without a runtime manifest (a vertex fetch, a typed target, a binding the manifest does
not describe) is refused before anything is compiled or published. Every refusal is logged as
`pipeline <key> not compiled in-process (<stage> stage): <reason>` and leaves no `<key>.txt`. The stage is
`request` (the files or their key), `vertex`, `pixel`, `pair` or `publish`. The pair translator does not say
which stage it refused, so for a pair of containers the job asks each stage on its own and names the one that is
refused for the same reason. The compile is tried once per key. A published `<key>.txt` is used when one
exists: the first draw looks for it again before compiling, and the job removes and replaces nothing that was
published.

## Pair and controls

The pair is the one [`native_startup_request_cache_check.py`](../../tests/native_startup_request_cache_check.py)
builds: a vertex container that covers the viewport from the vertex index and a pixel container that exports
one constant colour (red, `ff0000ff`). The consumer clears an 8x4 `R8G8B8A8_UNORM` target to blue (`0000ffff`),
sets the viewport and scissor to the left half and draws three vertices. The inside pixel is (1, 1) and the
outside pixel is (6, 1). The outside pixel is outside the viewport, not outside a triangle edge.

1. Skipped draw: the consumer records the list without the draw (`X2W_TEST_NO_DRAW`). Both pixels keep the clear
   colour and the cache holds request files only, so creating the pipeline compiles nothing.
2. Switch off: the draw is recorded without `X2W_NATIVE_SHADERS`. Nothing is compiled, the log says
   `no translation for pipeline <key>` and both pixels keep the clear colour.
3. First draw: the log names the key and the published manifest
   (`pipeline 35bc94721d5500d2 compiled in-process: x2w-manifest 1;ud 1;udsrc 0 rect;params 0;targets 0`), the
   loaded translation (1 entry value, 0 resources), the desktop pipeline for one target of format 28 with
   `0x00000000`, and the call as `drawn`. After the fence the inside pixel is red, the outside pixel is the clear
   colour and all 32 pixels match. The cache holds the request, `native` and the three `<key>` files, with the
   manifest not older than the stages.
4. Second process: the same 32 pixels. The log has the cache-entry line and no compile line, and no file in the
   cache changed (names, sizes and modification times, `native` included).
5. Published before the first draw: a copy of that cache without its `<key>.txt`. The consumer moves the
   manifest into place after it created the pipeline and before it draws (`X2W_TEST_PUBLISH_BEFORE_DRAW`). The
   log has `no translation` at creation, then the cache-entry line and no compile line. The same 32 pixels, no
   `native` directory, and the three `<key>` files keep their sizes and modification times.
6. Unsupported pairs, each from an empty cache. In every case both pixels keep the clear colour and the cache
   holds no `<key>.*` file. The cases come from `native_direct_container_check.py`:
   - a pixel stage that reads an entry register the compiler does not provide (`bary-pull`):
     `not compiled in-process (pixel stage): native compute +0x0: unsupported pixel entry VGPR 0`;
   - a vertex stage with an instruction before its vertex fetch (`late-fetch`):
     `(vertex stage): native compute +0x8: vertex fetch call outside the entry`;
   - a pixel stage that reads an interpolant the vertex stage does not export (`interpolated_pixel`):
     `(pair stage): native compute +0x0: graphics stages disagree on TEXCOORD0`.

## Command and result

```powershell
cmake --preset dev   # run here with -DINTAKE_PYTHON=<an existing out/pe-env python.exe>, the worktree having none
cmake --build --preset dev --target compile_dxil startup_stream_consumer offscreen_check d3d12_xs_shim
ctest --preset dev --no-tests=error -V -R '^(startup_draw_native|native_startup_request_cache|startup_compute_dispatch_native)_(warp|hardware)$|^native_startup_request_cache_controls$'
```

Commit `5639d957` on `cpp-migration` at `05438d65`, Windows 11 build 26300.9974, Debug build, D3D12 debug layer
enabled.

| Test | Adapter, driver | Result |
|---|---|---|
| `startup_draw_native_hardware` | NVIDIA GeForce RTX 4090, 32.0.16.1088 | PASS. Steps 1 to 6 as above. No debug-layer error message. |
| `startup_draw_native_warp` | Microsoft Basic Render Driver, 10.0.26100.9818 | PASS. Steps 1 to 6 as above. No debug-layer error message. |
| `native_startup_request_cache_controls` | none (no device) | PASS (the tool's request directory job, now the shared function). |
| `native_startup_request_cache_hardware` | NVIDIA GeForce RTX 4090, 32.0.16.1088 | PASS. |
| `native_startup_request_cache_warp` | Microsoft Basic Render Driver, 10.0.26100.9818 | PASS. |
| `startup_compute_dispatch_native_hardware` | NVIDIA GeForce RTX 4090, 32.0.16.1088 | PASS (the compute route under the same switch, unchanged). |

Capability skip, not a pass: `startup_compute_dispatch_native_warp` exits 77 on the Microsoft Basic Render
Driver 10.0.26100.9818, which reports no 32-lane waves. It skips the same way at `05438d65`. The graphics pair
needs no 32-lane waves and has no capability skip on WARP.

Baseline: at `05438d65` the same five existing tests give the same results (four pass, the one skip). The
first version of the new check (steps 1 to 4 and the pixel-stage refusal), run against that build on both
adapters, passes steps 1 and 2 and fails step 3 with the inside pixel at the clear colour
(`inside 0000ffff outside 0000ffff`): that commit has no in-process graphics route.

Mutation: with the look for a published `<key>.txt` removed from the first draw, `startup_draw_native_warp`
fails. Steps 1 to 4 do not depend on that line.

## Limits

- One pair was drawn: no resources, no samplers, no root constants, one `R8G8B8A8_UNORM` target, a pixel stage
  without interpolants. Bindings other than `udsrc 0 rect` reach the same manifest loader but were not drawn
  through this route.
- Slot 86 with two containers only. A slot 57 pair reaches the same code and was not run; a raw pair that needs
  a `.psin` is refused, because the stand-in does not publish one. Mesh pipelines and RTV variants
  (`req_<key>.rtv`) are not compiled in-process.
- An indirect draw of a pair compiled this way was not run.
- The `request` and `publish` stages and the refusal of a pair without a runtime manifest have no fixture. The
  `vertex` and `pixel` stages were run as frontend refusals, not as DXC failures.
- A translator that publishes `<key>.txt` while the in-process compile of the same key is running is not
  detected: the stand-in's files replace it.
- The compile runs on the drawing thread with the draw lock held. Its cost on a start-up run was not measured.
- A refused key is retried in every process, since nothing records the refusal in the cache.
- Only the two adapters above were run, in a Debug build. No start-up run used the switch for graphics.
- Pull requests into `cpp-migration` run the Windows job, whose pull request preset does not include these
  tests, so the results above are local runs.
