# Defensive Coding Standards — The Rails Way in C23

**Rule: if the compiler can't enforce it, it will be violated.**

This document defines architectural enforcement patterns that make it
impossible for any contributor (human or AI agent) to accidentally skip
validation, swallow errors, or leak memory. Read this before writing
any new code.

---

## 1. Every write goes through AR_BEGIN_SAVE — no exceptions

**Problem:** `coins_view_sqlite.c` and `wallet_sqlite.c` call
`sqlite3_step()` directly with no validation. This is how we lost 1.3M
UTXOs on 2026-04-10.

**Enforcement:** Add a compile-time ban on raw `sqlite3_step` in
application code.

```c
/* In activerecord.h — poison raw sqlite3_step in app code */
#ifdef ZCL_AR_ENFORCE
  /* Any file that includes activerecord.h cannot call sqlite3_step directly.
   * Use AR_STEP_ROW() or AR_STEP_DONE() which go through the AR macros.
   * To opt out (e.g. in lib/storage internals), #define ZCL_AR_RAW_SQL
   * before including activerecord.h. */
  #ifndef ZCL_AR_RAW_SQL
    #define sqlite3_step(x) \
      _Pragma("GCC error \"Use AR_STEP_ROW/AR_STEP_DONE, not raw sqlite3_step\"")
  #endif
#endif
```

Files that legitimately need raw SQL (the storage primitives in
`lib/storage/src/coins_view_sqlite.c`) opt out with `#define
ZCL_AR_RAW_SQL`. The Makefile adds `-DZCL_AR_ENFORCE` globally. This
makes raw SQL a conscious, visible decision — not a silent default.

**Status: shipped (Wave 3).** The ratchet allowlist at
`tools/scripts/raw_sqlite_allowlist.txt` is empty. All production writes
across `app/models/src/`, `app/controllers/src/`, `app/services/src/`,
and `lib/wallet/src/wallet_sqlite.c` route through `AR_BEGIN_SAVE` /
`AR_STEP_ROW` / `AR_STEP_DONE`. `make lint` runs `check_raw_sqlite.sh`
as one of 10 mandatory gates.

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
      check-silent-errors-controllers
	@echo "All lint checks passed"

ci: lint test fuzz-ci coverage
```

**Status: 10 gates active.** `make ci` fails if any fire. An agent
that pushes code with raw malloc, silent errors, bypassed AR
validation, unpaired stderr diagnostics, or a critical model missing
its before_save hook gets a red build before any human sees it.

---

## Summary: How agents learn to follow the Rails way

1. **Compiler errors** for raw `sqlite3_step` (unless opted out)
2. **Type system** forces `struct zcl_result` with message on failure
3. **CI lint** catches raw malloc, silent returns, missing error bodies
4. **Macros** make the right thing easier than the wrong thing
5. **Before/after hooks** wired by default — agents see the pattern and follow it
6. **This document** in the repo root — agents read it on `cat DEFENSIVE_CODING.md`

The Rails philosophy isn't "write good code." It's "make it harder
to write bad code than good code." These 7 patterns achieve that in C23.
