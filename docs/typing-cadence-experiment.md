# Bounded 8/16ms publication experiment

Qualification only. Production remains its existing 8ms policy and no high-resolution opt-in. The qualification-only ProcessOptions budget accepts exactly 8 or 16ms; it changes only the normal dirty-frame publication interval. Input writes, byte feeds, terminal deadlines, final output, queue bounds and provider defaults are unchanged. No cursor/backspace heuristic or delayed input is introduced.

Hypothesis: System ConPTY's observed approximately 15ms split redraw may complete before a 16ms publication, reducing retreats versus high-resolution 8ms. This is not guaranteed: the publication interval is measured from the last frame, not from input, and genuine terminal events can still publish. Packaged may lose latency at 16ms. Report both providers and all cases, not only favorable ones.

Run exact branch `diagnostic/typing-cadence`. For each provider the driver runs one uninstrumented CV8 control, CV8 trace, highres8 trace, and highres16 trace. Every trace is independently native-replayed against the terminal engine, including exact prelude and command order, cursor/row hashes and immutable snapshots. All six replay input signatures must match. Declared budget and actual highres wake evidence gate each case; malformed/overflow/incomplete evidence fails, never retries. Trace remains bounded to 2048 records/1MiB.

Both highres candidates include the production-qualified queue-locked event prepare/notification fix and arm only for dirty publications. Native 512 producer/prepare races run. No comparison claims based on the earlier stale-notification implementation.

Capture every result and report: command-to-write, WriteFile-to-read, last-feed-to-publication, expected-cursor publication, scheduled/actual deadline, all publication retreats and whether output was queued, frame replay correctness and byte identity. Instrumentation affects scheduling; fixture echo sampling is not physical input-to-pixel latency or perceived smoothness. Do not enable a production budget or switch provider from this experiment without a separate decision.
