# Defensive Coding Standards — The Rails Way in C23

**Rule: if the compiler can't enforce it, it will be violated.**

This document defines architectural enforcement patterns that make it
impossible for any contributor (human or AI agent) to accidentally skip
validation, swallow errors, or leak memory. Read this before writing
any new code.

Modules prefixed `legacy_` are a compatibility layer with an external
`zclassicd`. See [`LEGACY_LIFECYCLE.md`](./LEGACY_LIFECYCLE.md) for
which paths are still load-bearing.

---

## 1. Every write goes through the AR lifecycle — no exceptions

**Problem:** `coins_view_sqlite.c` and `wallet_sqlite.c` historically
called `sqlite3_step()` directly with no validation. This is how we lost
1.3M UTXOs on 2026-04-10.

**Enforcement:** Compile-time ban on raw `sqlite3_step` in application
code, plus a CI lint that re-checks the same surface.

```c
/* In activerecord.h — poison raw sqlite3_step in app code */
#ifdef ZCL_AR_ENFORCE
  /* Any file that includes activerecord.h cannot call sqlite3_step directly.
   * Use the AR lifecycle macros instead. To opt out (e.g. in
   * lib/storage internals), #define ZCL_AR_RAW_SQL before include. */
  #ifndef ZCL_AR_RAW_SQL
    #define sqlite3_step(x) \
      _Pragma("GCC error \"Use the AR lifecycle macros, not raw sqlite3_step\"")
  #endif
#endif
```

**The three lifecycle entry points (all defined in `activerecord.h`):**

| Macro | When to use | What it does |
|-------|-------------|--------------|
| `AR_BEGIN_SAVE(cbs, name, rec, validate_fn)` + `AR_FINISH_SAVE(cbs, rec, ok)` | You build the statement yourself between the two macros (e.g. multi-statement transactions, conditional binds) | `AR_VALIDATE_RECORD` → `before_save` hook → your code → `after_save` hook → `return ok` |
| `AR_ADHOC_SAVE(ndb, stmt, sql, cbs, name, rec, validate_fn, bind_code)` | Single locally-prepared INSERT/UPDATE statement (the common case) | Wraps `AR_BEGIN_SAVE` + `AR_PREPARE_BOOL` + your bind block + `AR_FINALIZE_STEP_DONE` + `AR_FINISH_SAVE` |
| `AR_CACHED_SAVE(stmt, cbs, name, rec, validate_fn, bind_code)` | Hot path with a cached prepared statement already owned by `node_db` | Same lifecycle, skips the prepare — call `AR_RESET(stmt)` and bind |

All three invoke the same `validate_*` + `before_save` + `after_save`
chain, so model hooks fire identically regardless of which one the
caller picked. Pick the one that fits the call site.

**Minimal call-site (the common case — `AR_ADHOC_SAVE`):**

```c
bool db_wallet_key_save(struct node_db *ndb, const struct db_wallet_key *k) {
    if (!ndb->open) return false;
    wallet_key_init_hooks();
    struct ar_callbacks *cbs = db_wallet_key_callbacks();
    sqlite3_stmt *s = NULL;
    AR_ADHOC_SAVE(ndb, s,
        "INSERT OR REPLACE INTO wallet_keys(pubkey_hash,pubkey,privkey,"
        "compressed,created_at) VALUES(?,?,?,?,?)",
        cbs, "wallet_key", k, db_wallet_key_validate,
        AR_BIND_BLOB(s, 1, k->pubkey_hash, 20);
        AR_BIND_BLOB(s, 2, k->pubkey, (int)k->pubkey_len);
        AR_BIND_BLOB(s, 3, k->privkey, 32);
        AR_BIND_INT(s, 4, k->compressed ? 1 : 0);
        AR_BIND_INT(s, 5, k->created_at));
}
```

Files that legitimately need raw SQL (storage primitives in
`lib/storage/src/coins_view_sqlite.c`) opt out with `#define
ZCL_AR_RAW_SQL`. The Makefile adds `-DZCL_AR_ENFORCE` globally. Raw SQL
becomes a conscious, visible decision.

**Status: shipped.** The ratchet allowlist at
`tools/scripts/raw_sqlite_allowlist.txt` is empty. All production
writes across `app/models/src/`, `app/controllers/src/`,
`app/services/src/`, and `lib/wallet/src/wallet_sqlite.c` route through
the AR lifecycle (one of the three macros above). `make lint` runs
`check_raw_sqlite.sh` as gate #3 of 11.

---

## 2. Every function that can fail returns a result type — not bare bool

**Problem:** `return false` with no context. Caller has no idea why.

**Enforcement:** Standard result type for all service/model functions.

```c
/* lib/util/include/util/result.h */

struct zcl_result {
    bool        ok;
    int         code;          /* 0 = success, negative = error category */
    char        message[256];  /* human-readable, always populated on failure */
    const char *source_file;   /* __FILE__ */
    int         source_line;   /* __LINE__ */
};

#define ZCL_OK ((struct zcl_result){.ok = true, .code = 0})

#define ZCL_ERR(err_code, fmt, ...) ((struct zcl_result){ \
    .ok = false, \
    .code = (err_code), \
    .source_file = __FILE__, \
    .source_line = __LINE__, \
    .message = "" \
})
/* message populated via snprintf in the macro expansion */

#define ZCL_CHECK(result) do { \
    struct zcl_result _r = (result); \
    if (!_r.ok) { \
        log_json("error", "zcl_check_failed", \
                 "code", _r.code, \
                 "message", _r.message, \
                 "file", _r.source_file, \
                 "line", _r.source_line); \
        return _r; \
    } \
} while (0)
```

**Rule:** New service functions MUST return `struct zcl_result` instead
of `bool`. Existing code migrates incrementally.

**Why this works for agents:** An agent that writes `return false;` in a
function declared as returning `struct zcl_result` gets a compiler error.
They MUST write `return ZCL_ERR(-1, "reason: %s", detail);` which forces
them to explain the failure.

---

## 3. Every malloc is checked — use zcl_malloc or die

**Problem:** 15+ unchecked malloc/calloc calls in sync services.
Silent NULL dereference.

**Enforcement:**

```c
/* lib/util/include/util/safe_alloc.h */

/* Checked malloc — logs and returns NULL (caller must handle).
 * Use when graceful degradation is possible. */
static inline void *zcl_malloc(size_t size, const char *label)
{
    void *p = malloc(size);
    if (!p && size > 0) {
        log_json("error", "malloc_failed",
                 "size", (int64_t)size,
                 "label", label);
        event_emitf(EV_OOM, 0, "label=%s size=%zu", label, size);
    }
    return p;
}

/* Checked malloc — aborts on failure.
 * Use when there's no reasonable fallback. */
[[noreturn]] static inline void zcl_oom_abort(size_t size, const char *label);

static inline void *zcl_malloc_or_die(size_t size, const char *label)
{
    void *p = malloc(size);
    if (!p && size > 0) zcl_oom_abort(size, label);
    return p;
}

/* Checked realloc — never leaks the original pointer. */
static inline void *zcl_realloc(void *ptr, size_t size, const char *label)
{
    void *p = realloc(ptr, size);
    if (!p && size > 0) {
        log_json("error", "realloc_failed",
                 "size", (int64_t)size,
                 "label", label);
        /* Original ptr is NOT freed — caller decides. */
    }
    return p;
}
```

**Makefile enforcement:**

```makefile
# In CI / make lint:
check-malloc:
	@echo "Checking for raw malloc/calloc/realloc..."
	@grep -rn '\bmalloc\b\|bcalloc\b\|\brealloc\b' \
	    app/ lib/ config/ tools/ \
	    --include='*.c' \
	    | grep -v 'zcl_malloc\|zcl_calloc\|zcl_realloc' \
	    | grep -v 'safe_alloc.h' \
	    | grep -v 'vendor/' \
	    | grep -v '// raw-alloc-ok' \
	    && echo "FAIL: use zcl_malloc/zcl_calloc/zcl_realloc" && exit 1 \
	    || echo "OK: no raw allocations"
```

Files that need raw malloc (e.g. vendor code, allocator internals) add
`// raw-alloc-ok` comment on the line.

---

## 4. Every error path logs with context — use LOG_ERR macro

**Problem:** 100+ `return -1;` or `return false;` with no logging.

**Enforcement:**

```c
/* lib/util/include/util/log_macros.h */

/* Log + return false */
#define LOG_FAIL(domain, fmt, ...) do { \
    log_json("error", domain, \
             "file", __FILE__, \
             "line", __LINE__, \
             "func", __func__, \
             ##__VA_ARGS__); \
    return false; \
} while (0)

/* Log + return -1 (for MCP handlers) */
#define LOG_ERR(domain, fmt, ...) do { \
    log_json("error", domain, \
             "file", __FILE__, \
             "line", __LINE__, \
             "func", __func__, \
             ##__VA_ARGS__); \
    return -1; \
} while (0)

/* Log + return custom value */
#define LOG_RETURN(val, domain, fmt, ...) do { \
    log_json("error", domain, \
             "file", __FILE__, \
             "line", __LINE__, \
             "func", __func__, \
             ##__VA_ARGS__); \
    return (val); \
} while (0)
```

**CI lint rule:**

```makefile
check-silent-errors:
	@echo "Checking for silent error returns..."
	@grep -rn 'return -1;' app/ tools/mcp/ --include='*.c' \
	    | grep -v 'LOG_ERR\|log_json\|fprintf' \
	    && echo "FAIL: silent error returns found" && exit 1 \
	    || echo "OK: all error returns logged"
```

---

## 5. MCP handlers must log on every error path

**Problem:** silent `return -1;` in MCP handlers leaves the caller with
no diagnostic info.

**Status: enforced by lint.** `make lint` runs three controller-tier
gates:

- `check-silent-errors` — every bare `return -1;` in
  `tools/mcp/controllers/*.c` must either be preceded by a logging
  call (`LOG_ERR`, `log_json`, `fprintf`) or carry an explicit
  `// raw-return-ok:<reason>` marker (no space after the colon).
- `check-silent-errors-services` — same rule for `app/services/src/`.
- `check-silent-errors-controllers` — same rule for
  `app/controllers/src/`.

A future `mcp_fail(res, code, fmt, ...)` helper could enforce that
`res->body` is populated on every error path by return-type discipline,
but that's optional polish — the lint already prevents the silent-fail
class entirely.

---

## 6. Before/after save hooks — wired

**Status: shipped.** Every critical model wires `ar_register_before_save`
and `ar_register_after_save`. The `check-before-save-hooks` lint
enforces that `utxo`, `block`, `wallet_key`, and `wallet_tx` keep these
hooks — drop one and `make lint` fails.

| Model | before_save | after_save |
|-------|-------------|------------|
| wallet_key | Log if `ZCL_WALLET_PASSPHRASE` set (keystore owns at-rest wrap) | Emit `EV_WALLET_KEY_SAVED` (`EV_SAPLING_KEY_SAVED` for sapling rows) |
| utxo | Validate money range + script coherence | Update UTXO commitment cache |
| block | Validate hash matches header | Emit `EV_BLOCK_SAVED` |
| wallet_tx | Validate txid format | Emit `EV_WALLET_TX_SAVED` |
| mempool_entry | Validate fee + size envelope | (no after_save) |
| tx_index | Validate txid + block height | (no after_save) |

---

## 7. CI gates — the final enforcer

Add to `Makefile`:

```makefile
lint: check-malloc check-silent-errors check-raw-sqlite \
      check-raw-malloc check-coins-lookup-nullcheck \
      check-observability-pairing check-silent-errors-services \
      check-before-save-hooks check-pthread-create \
      check-silent-errors-controllers check-model-validation \
      check-long-functions
	@echo "All lint checks passed"

ci: lint test fuzz-ci coverage
```

**Status: 12 gates active.** `make ci` fails if any fire. An agent
that pushes code with raw malloc, silent errors, bypassed AR
validation, unpaired stderr diagnostics, a critical model missing
its before_save hook, a model file with no `validates_*` call and
no `ar-validate-skip:<tag>` marker, or a controller/service function
over 500 lines without a `long-function-ok:<tag>` override, gets a
red build before any human sees it.

### Gate #11: every model is either validated or explicitly skipped

`check-model-validation` walks every `app/models/src/*.c` and
requires one of:

1. At least one `validates_*` call (the macros from
   `app/models/include/models/activerecord.h` — `validates_presence_of`,
   `validates_range`, `validates_zcl_address`, etc.).
2. A top-of-file marker `ar-validate-skip:<tag>` (no space after the
   colon, non-empty tag) explaining why the AR validation lifecycle
   does not apply — e.g. `connection-handle-not-a-row`,
   `registry-module-not-a-row`, `shared-helpers-not-a-row`.

This pins the wave-6 result: validations are required for every row
model, and infrastructure / registry / helper files declare their
exemption in code rather than by silent omission. Implementation:
`tools/scripts/check_model_validation.sh`.

### Gate #12: controller / service functions stay under 500 lines

`check-long-functions` walks every `app/controllers/src/*.c` and
`app/services/src/*.c` and flags any top-level function whose body
spans more than 500 lines from signature to closing `}` on column 0.

Long functions are hard to test in isolation, hard to read in one
sitting, and almost always conceal two or more concerns waiting to
be split.  The two report builders that broke this cap before wave
7d — `explorer_factoids_build` (1389L, 17 archaeology sections) and
`explorer_stats_build` (1011L, 10 statistics sections) — have been
refactored into per-section emit helpers, each under ~120 lines.

**Override marker.** A single state machine that genuinely belongs
as one function may carry `// long-function-ok:<tag>` on its
signature line.  The tag must be a non-empty single token matching
`[A-Za-z][A-Za-z0-9_-]+` (same syntax as the other lint overrides)
and describe WHY the rule does not apply.

Implementation: `tools/scripts/check_long_functions.sh`.

---

## 8. Lint-override discipline — every escape hatch is named

Five lint gates accept an inline override marker when the underlying
rule cannot mechanically hold. The five marker classes:

| Marker | Where allowed | Lint gate |
|--------|---------------|-----------|
| `// obs-ok:<tag>` | line with `fprintf(stderr, ...)` whose nearby code does not emit an event or terminally propagate | `check-observability-pairing` |
| `// raw-sql-ok:<tag>` | line with `sqlite3_step(...)` outside the `AR_STEP_*` wrappers | `check-raw-sqlite` |
| `// raw-return-ok:<tag>` | bare `return -1;` in MCP / service / controller code with no preceding log line | `check-silent-errors`, `-services`, `-controllers` |
| `// raw-alloc-ok:<tag>` | line with `malloc/calloc/realloc` outside the `zcl_*` wrappers | `check-raw-malloc` |
| `// long-function-ok:<tag>` | signature line of a top-level controller/service function whose body spans >500 lines | `check-long-functions` |

**Syntax (machine-enforced).** Every marker requires a non-empty
single-token tag matching `[A-Za-z][A-Za-z0-9_-]+` immediately after
the colon. The space-after-colon form (`// raw-sql-ok: state-kv …`)
and the bare form (`// raw-alloc-ok`) are rejected by the lint —
hyphen-join multi-word tags instead.

**Pairing rule.** A marker is not a free pass; it is a promise that
the override is either:

1. **Logged at this site or one nearby** — the diagnostic is already
   observable (LOG_FAIL above, fprintf on the previous line, the
   caller logs on receiving the propagated failure).
2. **Structurally safe by design** — qsort comparator, void-returning
   helper, pre-boot sentinel, build-time tool, test fixture.

If neither holds, the marker is a bug. Delete it, fix the underlying
issue (route through `AR_BEGIN_SAVE`, add `LOG_FAIL`, switch to
`zcl_malloc`), and let the lint go green naturally.

**Prefer reusable tags that name a structural property over one-off
labels.** `:debug` and `:operator` say nothing about why the call is
safe; `:helper-context-logged` and `:bin-parser-bounds` describe a
class of safe call sites that a future reader can recognize. When the
override pattern at hand matches one already in use (see taxonomy
below), reuse that tag rather than coining a fresh one. Singleton tags
should only survive when they name a genuinely unique structural
property (e.g. `fatal-true-triggers-rollback-and-partial-write-return`)
— ad-hoc labels get folded back into the shared vocabulary.

**Concrete tag taxonomy (existing usage at wave-7c):**

- `obs-ok:` — `pre-existing-diagnostic`, `helper-context-logged`,
  `helper-return-path`, `paired-with-return-false-below`,
  `paired-with-event_emitf-below`, `warning-only-on-best-effort-path`,
  `crash-dump-banner`.
- `raw-sql-ok:` — `kv-state-primitive`, `read-only-introspection`,
  `state-kv-write-caller-handles-rc`, `cvs-zcl-ar-raw-sql-rationale`,
  `test-fixture-setup`, `test-fixture-verify`, `standalone-dev-tool`.
- `raw-return-ok:` — `qsort-comparator`, `logged-above`, `sentinel`,
  `bin-parser-bounds`, `sentinel-no-compile-time-windows`.
- `raw-alloc-ok:` — `test-fixture`, `standalone-dev-tool`,
  `db-service-owns-heap-job`.
- `long-function-ok:` — `legacy-import-state-machine`.

Implementation: `tools/check_observability_pairing.c`,
`tools/scripts/check_raw_sqlite.sh`,
`tools/scripts/check_raw_malloc.sh`,
`tools/scripts/check_long_functions.sh`, and the inline
`check-silent-errors*` recipes in `Makefile:654+`.

---

## Summary: How agents learn to follow the Rails way

1. **Compiler errors** for raw `sqlite3_step` (unless opted out)
2. **Type system** forces `struct zcl_result` with message on failure
3. **CI lint** catches raw malloc, silent returns, missing error bodies,
   long-function bloat
4. **Macros** make the right thing easier than the wrong thing
5. **Before/after hooks** wired by default — agents see the pattern and follow it
6. **This document** in the repo root — agents read it on `cat DEFENSIVE_CODING.md`

The Rails philosophy isn't "write good code." It's "make it harder
to write bad code than good code." These 8 patterns achieve that in C23.
