# Decider tools

Scripts that turn a trace store into a decider-example dataset (RFD66606-2334,
`urn:oid:1.3.6.1.4.1.66606.1.2.2334`). They read a trace store of schema 5, XR Pilot's or
cua-tracer's: sessions, spans and views carry the UUIDv7 the store gave them, so a rebuild keeps
every id.

| Script | What it does |
|---|---|
| `build_sample.exs <store> <stage> <recorder repo> <recorder commit>` | Writes `etnf/*.parquet` (the store's tables, keyed by UUIDv7 or by name) and `decider_denormalized.parquet` (one row per example and question, for the dataset viewer), all zstd. It refuses a store whose keys do not hold. |
| `check_sample.exs <stage> <store>` | Rechecks a built sample against its store from the files alone: zstd in every Parquet footer, UUIDv7 keys that are the store's, and view bytes and gold counts that match. Three planted faults must be caught. |

XR Pilot asks no Choice question (it measures travel and change, which are numbers), so its rows
have views, poses and measures and no gold. cua-tracer's rows also carry the `effect` question.

CI (`ci / linux-core`) builds a sample from `decider-sample.sqlite`, which the core test
`traces.decider-sample` writes, and checks it.
