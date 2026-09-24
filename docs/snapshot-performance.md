# Snapshot construction performance

The `patch/perf-elements` branch keeps the 2.3.2 wire API and per-frame snapshot
freshness. It changes snapshot construction in four places:

- Index previous `(instance_id, generation)` pairs once instead of searching all
  old nodes for every new node. Duplicate identities retain the first creation
  sequence, as before; key equality checks the full string and generation.
- Discover cameras during the existing depth-first traversal. Resolve bounds
  afterwards in reverse node order, with children available before parents.
  Native scene handles are retained only within that synchronous build.
- Store sampled property values and serialize them only when included in a
  response. Strings are copied and numeric/vector values are owned by the
  snapshot; a subsequent richer query can still return every property.
- Compare native property-name hashes directly while building nodes. Convert
  names to strings only when returning properties.

No frame throttling, selector-specific cache, long polling, or game-specific
shortcut is introduced. The Python wrapper is unchanged.

## Measurement

Windows, Defold Editor 1.13.1, OpenGL, Shining Sword `full_world` recorded save,
1920x1080 drawable with a 960x540 project reference size. After three seconds of
warmup, measure three cycles of four-second idle/state/elements/idle windows.
Use engine `/frame` counters and host monotonic time for FPS. Sleep 50 ms on the
host after each synchronous query (200 ms for idle windows):

```python
game.state('ld52.farm')
game.elements(name='controls_keyboard', include='basic,bounds', limit=30)
```

| Native build | Median idle FPS | Median state FPS | Median elements FPS | Mean elements latency |
| --- | ---: | ---: | ---: | ---: |
| Upstream 2.3.2 | 58.78 | 58.94 | 29.74 | 117 ms |
| This patch, local Editor build | 59.80 | 60.03 | 57.66 | 40 ms |

Element polling retains 96% of adjacent idle FPS, with unchanged query cadence.
Profiler capture is a separate run. The added `BridgeSnapshot` scope measured
25.08 ms mean / 29.92 ms p95 total time; upstream had no equivalent scope, so
its 90.37 ms service self-time p95 is not a directly comparable scope.

## Regression coverage

`AutomationBridgeApiTest.test_snapshot_properties_and_identity_survive_basic_queries`
checks basic then rich responses, stable creation sequences, shared component
identity, pagination order, strings, booleans and vectors. Its read-only helper
in `tests/snapshot_checks.py` can also run against a larger game with a stable
scene. Existing sample runtime tests cover parent/component bounds and input
guards. Run those through the Editor workflow documented in `DEVELOPMENT.md`.

The patch was built in Shining Sword's Editor. The dependency-free suite passed
227 tests. The shared snapshot helper passed against its full world. Seven
Editor game tests passed (farm and photobooth, animated GUI bounds and clicks,
save restoration, skeleton, player resume, titles and reboot/recovery). The full
sample runtime suite and native platforms other than Windows are not claimed
as verified by this measurement.
