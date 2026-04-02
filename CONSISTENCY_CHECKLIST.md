# Consistency Refactor Checklist

## Goal

Make the codebase more predictable across layers by standardizing:

- where SQL lives
- how SQLite statements are executed
- how runtime state is passed
- how large controllers/services are split
- which helpers/macros are the preferred default

This is a consistency program, not a one-shot cleanup. Each slice should
improve one repeated pattern without broad, risky rewrites.

## Principles

- prefer boundary consistency over adding more macros by default
- move persistence ownership toward models and read-model helpers
- keep controllers thin: parse input, call helpers/services, format output
- keep services workflow-oriented, not schema-owning
- use small helpers when they clarify lifecycle; use macros only for narrow,
  repeated boilerplate
- avoid parallel abstractions for the same problem

## Checklist

### 1. Controller Query Ownership

- [ ] audit controller-owned read SQL and group it by domain
- [ ] extract wallet-view read queries into wallet projection/read helpers
- [ ] extract explorer token/address/block stats reads into explorer read helpers
- [ ] extract API aggregate/stat queries into dedicated read helpers
- [ ] remove duplicated scalar query patterns once helper coverage exists

Exit criteria:

- controllers mostly call read helpers instead of owning multi-query report SQL

### 2. SQLite Call Pattern Standardization

- [ ] standardize read-only scalar/list query helpers for controllers/services
- [ ] standardize transaction helpers outside model/test code where repetition exists
- [ ] replace hand-rolled prepare/step/finalize blocks in low-risk files first
- [ ] keep dynamic SQL behind narrow helpers with explicit whitelisting

Exit criteria:

- raw SQLite lifecycle code follows one small set of recognizable patterns

### 3. Wallet View Consistency

- [ ] consolidate wallet-view query helpers behind one projection-oriented surface
- [ ] replace page-local formatting drift with shared formatting helpers
- [ ] reduce page-local globals by moving wallet-view state into an explicit context
- [ ] keep wallet view page handlers focused on rendering and request handling

Exit criteria:

- wallet view pages share one query/formatting vocabulary

### 4. Explorer And API Surface Split

- [ ] split `explorer_controller.c` by resource area
- [ ] split `explorer_factoids.c` by factoid/stat domain where practical
- [ ] split `api_controller.c` by API resource or feature group
- [ ] move reusable read logic into shared explorer/API helpers before splitting

Exit criteria:

- no single explorer/API controller acts like a subsystem dump

### 5. Runtime Context Cleanup

- [ ] inventory remaining controller/service globals that should be explicit state
- [ ] move wallet-view globals behind a dedicated runtime/context object
- [ ] keep `config/` as composition-only by shrinking remaining ambient globals
- [ ] convert service-local file-static counters/timers to explicit state where useful

Exit criteria:

- new code reaches dependencies through contexts by default, not ambient globals

### 6. Bulk Import / Rebuild Helpers

- [ ] extract repeated PRAGMA/index/drop/create sequences into dedicated helpers
- [ ] table-drive repetitive index rebuild/drop lists where it improves clarity
- [ ] consolidate repeated import transaction scaffolding
- [ ] keep high-volume import loops readable and explicit

Exit criteria:

- import/rebuild code is repetitive because of data shape, not because of setup noise

## Recommended Execution Order

1. wallet-view helper consistency
2. controller read-helper extraction
3. explorer/API splits
4. runtime context cleanup
5. bulk import helper cleanup

## Current Slice

In progress:

- keep the consistency work tracked as an explicit step-by-step program
- extend shared controller SQLite helpers beyond scalar-only reads
- replace low-risk explorer/API prepare-step-finalize boilerplate with those helpers
- consolidate repeated explorer/API domain summaries behind shared read helpers

Recent slices:

- added wallet-view projection helpers for send/receive page queries
- standardized shared test DB macros across repeated fixture/setup code
- started explorer/API read-helper extraction with shared text and fixed-width
  integer row helpers
- started consolidating repeated ZSLP summary reads behind one shared helper
- started consolidating repeated address/privacy summaries behind shared helpers
- started consolidating repeated UTXO summaries behind shared helpers
- started consolidating repeated OP_RETURN and transaction summaries behind shared helpers
