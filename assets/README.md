# Assets

Real AS350 B3 recordings and the tables that describe them. Nothing here is
synthesised; see ../docs/assets.md for how each file is produced.

    wav/       recordings and loops (WAV, float32 or PCM, any sample rate)
    anchors/   one JSON table per start recording

An anchor table names the recording it belongs to in its `asset` field, and the
loader refuses a table whose last anchor lies past the end of that recording.
Rebuild the table whenever the recording is re-trimmed.
