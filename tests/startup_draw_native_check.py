"""A VS/PS pair compiled in the stand-in on its first draw, with the C++ compiler (#2978).

usage: startup_draw_native_check.py <startup_stream_consumer> <DXC directory> <warp|hardware>

X2W_NATIVE_SHADERS names the directory of dxcompiler.dll and dxil.dll. No offline translator runs and no stage
bytecode is prebuilt: every cache below starts empty. The pair is the source-built one of
native_startup_request_cache_check.py: a vertex stage that covers the viewport from the vertex index and a pixel
stage that exports one constant colour. The consumer creates it through device slot 86, clears an 8x4 RGBA8 target
to blue, sets the viewport and scissor to the left half and draws three vertices. INSIDE is a pixel of the left
half and OUTSIDE one of the right half (outside the viewport, not outside a triangle edge).

1. Skipped draw: the consumer records everything but the draw (X2W_TEST_NO_DRAW). Both pixels keep the clear
   colour, and the cache holds request files only: creating the pipeline compiles nothing.
2. Switch off: the draw is recorded without X2W_NATIVE_SHADERS. Nothing is compiled, the draw has no translation
   and both pixels keep the clear colour.
3. First draw: the log names the pair key, the in-process compile with the manifest it published (the bindings),
   the loaded translation, the desktop pipeline (0x00000000) and the drawn call. After the fence INSIDE is the
   shader's red and OUTSIDE the clear colour. The cache holds the request, the compiler's own cache (native) and
   <key>.vs.cso, <key>.ps.cso and <key>.txt, the manifest written last.
4. Second process, same cache: the same pixels, the log says the cache entry was loaded and no compile ran, and
   no file in the cache changed (names, sizes and modification times, the compiler's cache included).
5. Published before the first draw: a copy of that cache without its <key>.txt, which the consumer moves into
   place after it created the pipeline (X2W_TEST_PUBLISH_BEFORE_DRAW). The first draw loads it: the same pixels,
   no compile, no compiler cache, and the three <key> files keep their sizes and modification times.
6. Unsupported pairs, each from an empty cache: a pixel stage that reads an entry register the compiler does not
   provide (pixel stage), a vertex stage with an instruction before its vertex fetch (vertex stage), and a pixel
   stage that reads an interpolant the vertex stage does not export (pair stage). Each refusal names its stage
   and the reason, the draw has no translation, both pixels keep the clear colour and the cache holds no <key>.*
   file.

Exit 77 when the adapter is not available.
"""
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

from native_direct_container_check import interpolated_pixel, patched_pixel, vertex
from native_startup_request_cache_check import stream_pixel_container, stream_vertex_container

WIDTH, HEIGHT = 8, 4
RED, CLEAR = bytes((255, 0, 0, 255)), bytes((0, 0, 255, 255))
INSIDE, OUTSIDE = (1, 1), (6, 1)
DRAWN = (RED * 4 + CLEAR * 4) * HEIGHT
UNTOUCHED = CLEAR * WIDTH * HEIGHT
COMPILED, CACHE_HIT, REFUSED = 'compiled in-process: ', 'cache entry loaded, no in-process compile', 'not compiled in-process'


def at(image, point):
    offset = 4 * (point[1] * WIDTH + point[0])
    return image[offset:offset + 4]


def lines(result, text):
    return [line for line in result.stderr.splitlines() if text in line]


def published(cache):
    return sorted(p.name for p in cache.iterdir() if not p.name.startswith('req_') and p.name != 'native')


def snapshot(cache):
    return sorted((str(p.relative_to(cache)), p.stat().st_size, p.stat().st_mtime_ns) for p in cache.rglob('*') if p.is_file())


def draw(directory, consumer, adapter, env, cache, pixel_stage, vertex_stage=None, **extra):
    cache.mkdir(exist_ok=True)
    (directory / 'vs').write_bytes(vertex_stage or stream_vertex_container())
    (directory / 'ps').write_bytes(pixel_stage)
    pixels = directory / 'pixels'
    pixels.unlink(missing_ok=True)
    result = subprocess.run([consumer, directory / 'vs', directory / 'ps', directory / 'root', adapter, pixels],
                            env=dict(env, X2W_SHADER_CACHE=str(cache), **extra), capture_output=True, text=True)
    if result.returncode == 77:
        print(result.stdout + result.stderr[-2000:], end='')
        print(f'startup_draw_native: SKIP on {adapter}: the adapter is not available')
        sys.exit(77)
    assert result.returncode == 0, f'consumer exit {result.returncode}\n{result.stderr[-3000:]}'
    errors = [line for line in lines(result, 'x2w-d3d12-debug ') if ' sev=0 ' in line or ' sev=1 ' in line]
    assert not errors, errors[0]
    image = pixels.read_bytes()
    assert len(image) == len(DRAWN), len(image)
    key = [p.name[4:20] for p in cache.glob('req_*.a')]
    assert len(key) == 1, key
    return image, result, key[0]


def main(consumer, dxc_dir, adapter):
    with tempfile.TemporaryDirectory() as temporary:
        directory = Path(temporary)
        env = dict(os.environ, X2W_D3D12_PERMISSIVE='1', X2W_D3D12_LIST_NOOP='1', X2W_D3D12_RENDER='1',
                   X2W_D3D12_DEBUG='1', X2W_NATIVE_SHADERS=dxc_dir)
        env.pop('X2W_TEST_NO_DRAW', None)
        red = stream_pixel_container(False)
        tail = lambda result: result.stderr[-3000:]

        image, result, _ = draw(directory, consumer, adapter, env, directory / 'skipped', red, X2W_TEST_NO_DRAW='1')
        print(result.stdout, end='')
        assert image == UNTOUCHED, f'a list without a draw changed the target: {image.hex()}'
        assert not lines(result, 'in-process') and not published(directory / 'skipped'), tail(result)

        off = {name: value for name, value in env.items() if name != 'X2W_NATIVE_SHADERS'}
        image, result, key = draw(directory, consumer, adapter, off, directory / 'off', red)
        assert image == UNTOUCHED, f'a draw without a translation changed the target: {image.hex()}'
        assert not lines(result, 'in-process') and not published(directory / 'off'), tail(result)
        assert lines(result, f'no translation for pipeline {key}'), tail(result)

        cache = directory / 'cache'
        image, result, key = draw(directory, consumer, adapter, env, cache, red)
        assert at(image, INSIDE) == RED and at(image, OUTSIDE) == CLEAR and image == DRAWN, \
            f'inside {at(image, INSIDE).hex()} outside {at(image, OUTSIDE).hex()} image {image.hex()}\n{tail(result)}'
        manifest = (cache / f'{key}.txt').read_text(encoding='ascii')
        compiled = lines(result, COMPILED)
        assert len(compiled) == 1 and compiled[0].endswith(f'pipeline {key} {COMPILED}' + manifest.strip().replace('\n', ';')), tail(result)
        assert manifest.startswith('x2w-manifest 1\nud 1\nudsrc 0 rect\n'), manifest
        assert len(lines(result, f'pipeline {key} translated: 1 entry values, 0 resources')) == 1, tail(result)
        assert len(lines(result, f'desktop pipeline for {key}, 1 targets (format 28) -> 0x00000000')) == 1, tail(result)
        assert len(lines(result, f'pipeline {key} topology 0x4 -> drawn;')) == 1, tail(result)
        assert not lines(result, REFUSED) and not lines(result, CACHE_HIT), tail(result)
        names = [f'{key}.ps.cso', f'{key}.txt', f'{key}.vs.cso']
        assert published(cache) == names and (cache / 'native').is_dir(), published(cache)
        assert all((cache / name).read_bytes().startswith(b'DXBC') for name in (names[0], names[2]))
        assert (cache / names[1]).stat().st_mtime_ns >= max((cache / name).stat().st_mtime_ns for name in names)
        layer = lines(result, 'x2w-d3d12 debug layer: ')
        print('debug layer: ' + ('enabled, no error message' if layer and '0x00000000' in layer[0] else 'unavailable'))
        print(f'pair {key}: {compiled[0].split(COMPILED)[1]}')

        before = snapshot(cache)
        image, result, again = draw(directory, consumer, adapter, env, cache, red)
        assert again == key and image == DRAWN, f'{again} {image.hex()}\n{tail(result)}'
        assert len(lines(result, f'pipeline {key}: {CACHE_HIT}')) == 1 and not lines(result, COMPILED), tail(result)
        assert len(lines(result, f'pipeline {key} topology 0x4 -> drawn;')) == 1, tail(result)
        assert snapshot(cache) == before, 'the second process wrote to the cache'

        late = directory / 'late'
        late.mkdir()
        for name in names + [f'req_{key}.a', f'req_{key}.b', f'req_{key}.root']:
            shutil.copy2(cache / name, late / (name + '.pending' if name.endswith('.txt') else name))
        before = [(name.replace('.pending', ''), size, time) for name, size, time in snapshot(late)]
        image, result, again = draw(directory, consumer, adapter, env, late, red,
                                    X2W_TEST_PUBLISH_MANIFEST=str(late / f'{key}.txt'), X2W_TEST_PUBLISH_BEFORE_DRAW='1')
        assert again == key and image == DRAWN, f'{again} {image.hex()}\n{tail(result)}'
        assert lines(result, f'no translation for pipeline {key}'), tail(result)
        assert len(lines(result, f'pipeline {key}: {CACHE_HIT}')) == 1 and not lines(result, COMPILED), tail(result)
        assert snapshot(late) == before, 'the first draw replaced a translation published after pipeline creation'

        late_fetch = vertex(fetch=True, program=[0x7e000080, 0x7e0002f2, 0xbe842104, 0xf80008cf, 0x04030201, 0xbf810000])
        refusals = []
        for stage, reason, pixel_stage, vertex_stage in (
                ('pixel', 'unsupported pixel entry VGPR', patched_pixel(1, [0x7e000300], input_address=8), None),
                ('vertex', 'outside the entry', red, late_fetch),
                ('pair', 'graphics stages disagree on TEXCOORD', interpolated_pixel(), None)):
            refused = directory / f'refused-{stage}'
            image, result, key = draw(directory, consumer, adapter, env, refused, pixel_stage, vertex_stage)
            logged = lines(result, REFUSED)
            assert image == UNTOUCHED, f'a draw of a pair refused in its {stage} stage changed the target: {image.hex()}'
            assert len(logged) == 1 and f'pipeline {key} {REFUSED} ({stage} stage): ' in logged[0] and reason in logged[0], \
                f'{stage}: {logged}\n{tail(result)}'
            assert not lines(result, COMPILED) and not published(refused), published(refused)
            refusals.append(logged[0].split(REFUSED)[1].strip())

        print(f'startup_draw_native: PASS on {adapter} (first draw from an empty cache: inside {RED.hex()}, outside '
              f'{CLEAR.hex()}; second process: same pixels from the cache entry, no file changed; a translation '
              f'published before the first draw is loaded, not replaced; skipped draw and switch off: clear colour at '
              f'both points, nothing compiled; unsupported pairs, no cache entry: ' + '; '.join(refusals) + ')')


if __name__ == '__main__':
    if len(sys.argv) != 4 or sys.argv[3] not in ('warp', 'hardware'):
        sys.exit(__doc__)
    main(*sys.argv[1:])
