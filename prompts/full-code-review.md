# Full repository review prompt

Review the completed repository as a whole for correctness, regressions, security, architecture, public-contract impact, build/install/package leakage and meaningful simplification. Use archived reports, footprints and debt records as evidence, but do not replace change-level Evaluation or silently change active artifacts.

## Mandatory deliverable: orphan-interface scan

Always produce a dedicated `Orphan interfaces` section, separate from the correctness and quality findings. Scan the whole repository, not only the changed diff.

An orphan is a reviewed production surface with no real consumer. Treat each of these as a candidate:

- internal API, class or method declared for cross-file use whose only caller is a test or fixture;
- external API, exported symbol, header or C ABI entry with no representative downstream consumer;
- callback or plugin hook that is registered but never dispatched, or dispatched but never registered;
- CLI subcommand, flag or entrypoint that nothing invokes;
- configuration key or environment variable that is parsed but never consulted by a live code path;
- protocol or persistence shape with only one side implemented;
- build target, install rule, package rule or distribution surface with no downstream consumer;
- eBPF program compiled but never loaded, loaded but never read, or attached but never producing output;
- build or deploy script superseded by another entrypoint and no longer referenced;
- OpenSpec change or harness artifact left terminal, aborted or unreachable.

For each candidate, establish the evidence before reporting it:

1. Name the exact path and symbol, with `file:line`.
2. Show the search that proves the absence of a consumer, and state the scope searched.
3. Classify the consumer situation explicitly — `no caller`, `test-only caller`, `dead dispatch`, `parsed-never-read`, `superseded-entrypoint` or `unreachable-artifact`.
4. Verify it is real production code, not a generated file, vendor copy or removable test asset.

Report each surviving finding with severity (`Critical` / `Important` / `Minor`), the concrete consequence of leaving it, and a single recommended disposition: remove, replan with a real consumer, or document as intentional incubation with its exit condition. Order the section by severity.

Do not report a candidate you could not verify, and do not pad the section with weak findings. An empty section is a valid result — say so explicitly rather than listing near-misses.

The scan is read-only. Propose dispositions; never delete, deprecate or re-plan a surface yourself. Removing or changing a surface that carries an approved requirement, contract role or evidence obligation returns to Planner.
