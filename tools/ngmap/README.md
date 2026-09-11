# ngmap

Offline analysis of real AS350B3 recordings for the sound system. Produces the
artefacts the C++ module loads at run time: the NG% -> recording-time anchor
table for a start, and trimmed, seamless loops for the steady phases.

Nothing here synthesises audio. Every command takes recordings of the real
aircraft and returns either a description of them or a trimmed copy.

This is a member of the repository's uv workspace, so it is installed by
`uv sync` at the repository root - there is nothing to install here separately.

```bash
uv run ngmap analyze  recordings/cabin_start.wav --calibrate 67@41.5
uv run ngmap phases   recordings/flight.wav --extract work/
uv run ngmap anchors  recordings/cabin_start.wav --trace telemetry/start.csv \
                      --out assets/anchors/arriel2b1_start.json
uv run ngmap validate assets/anchors/arriel2b1_start.json recordings/cabin_start.wav \
                      --calibrate 67@41.5
uv run ngmap loop     work/03_steady_67.wav --region 4:28 --out assets/wav/idle_loop.wav
```

Dependencies: numpy and scipy. `uv sync --extra plots` adds matplotlib, which is
only ever used for looking at tracks by eye.

The full workflow, with what each command decides and why, is in
[../../docs/assets.md](../../docs/assets.md).
