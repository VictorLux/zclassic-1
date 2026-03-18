# ZClassic23: A Sovereign Internet

## What We Have

ZClassic gives us:
- **Proof-of-work consensus** — permissionless, censorship-resistant
- **Shielded transactions** (Sapling zk-SNARKs) — private payments
- **ZSLP tokens** — create and transfer custom tokens on-chain
- **OP_RETURN data** — store arbitrary data on the blockchain

ZClassic23 adds:
- **Embedded Tor** — every node is a hidden service, zero friction
- **Fast sync** — full node in 60 seconds via UTXO snapshot
- **MVC framework** — database-backed web apps in pure C23
- **On-chain DNS** — ZSLP tokens store .onion addresses
- **26MB single binary** — zero system dependencies

Together: **a platform for building and selling services, paid in private cryptocurrency, hosted on infrastructure no one can shut down.**

## The Product

### For Site Operators
```
./zclassic23 -tor
```
That's it. You now host a .onion website backed by SQLite, with shielded payment processing built in. No server rental. No domain registration. No SSL certificates. No payment processor. No bank account.

### For Customers
Open `zcl-browser`. Browse the directory. Find a service. Pay with shielded ZCL. Receive ZSLP tokens. Use the service. No account. No email. No identity.

### For Developers
Build MVC apps in C23. Define models (SQLite tables). Write controllers (route handlers). Render views (HTML templates). Accept payments (shielded ZCL). Issue tokens (ZSLP). Deploy by running the binary.

## Load Balancing

### The Problem
One .onion address = one Tor hidden service = one machine. If a service gets popular, one machine can't handle the load. Traditional load balancers (nginx, HAProxy) require clearnet infrastructure.

### The Solution: On-Chain Load Balancing

Every zclassic23 node that serves a site publishes a ZSLP token announcement:
```
ZSLP SEND: {
  token_id: <site_registry_token>,
  .onion: "zc23abc...xyz.onion",
  capacity: 100,        // max concurrent requests
  region_hash: <hash>,  // geographic hint (privacy-safe)
  version: 1            // site content version
}
```

Multiple nodes serve the same site content. The client (zcl-browser) discovers all nodes serving the same `token_id` from the blockchain, then:

1. **Random selection**: pick a random node from the set
2. **Latency probing**: connect to 3, use the fastest
3. **Failover**: if one is down, try the next
4. **Capacity-aware**: prefer nodes advertising higher capacity
5. **Version-consistent**: only connect to nodes with matching content version

### Architecture
```
Site "MyStore" (token_id: abc123)
  ├── Node A: zc23aaa...onion  (capacity: 200)
  ├── Node B: zc23bbb...onion  (capacity: 100)
  ├── Node C: zc23ccc...onion  (capacity: 50)
  └── Node D: zc23ddd...onion  (capacity: 100)

Client connects to zcl-browser:
  1. Scans chain for token abc123 SEND txs
  2. Finds 4 .onion addresses
  3. Probes latency to each via Tor
  4. Connects to fastest with capacity
  5. If fails, tries next
```

### Implementation

```c
// In the binary — auto load balancing
struct site_replica {
    char onion[68];
    int capacity;
    int version;
    int64_t last_seen;     // block height of announcement
    int64_t latency_us;    // measured round-trip via Tor
};

// Discover all replicas for a site
int site_discover_replicas(const char *token_id,
                            struct site_replica *out, size_t max);

// Select best replica (latency + capacity + freshness)
const char *site_select_replica(struct site_replica *replicas, int count);

// Announce this node as a replica
bool site_announce_replica(const char *token_id, int capacity, int version);
```

### Replication
Site operators run the same binary on multiple machines:
```bash
# Machine 1
./zclassic23 -tor -site=mystore -replicate=zc23aaa...onion

# Machine 2 (different machine, same content)
./zclassic23 -tor -site=mystore -replicate=zc23aaa...onion

# Machine 3
./zclassic23 -tor -site=mystore -replicate=zc23aaa...onion
```

Each announces itself on-chain. Clients discover all three. Load distributes automatically. If one goes down, the others continue. The site is never offline as long as one replica runs.

Content sync between replicas: the `content version` field in the ZSLP announcement ensures clients connect to nodes with the latest content. Site operators push updates to all replicas (rsync over Tor, or blockchain-anchored content hashes).

## ZSLP Token Commerce

### The Model
A SaaS store built on zclassic23:

1. **Merchant creates a ZSLP token** (e.g., "ACME_CREDITS")
2. **Customer pays shielded ZCL** to the merchant's z-address
3. **Merchant's node detects the payment** (z_listunspent polling)
4. **Merchant mints ZSLP tokens** and sends to customer's t-address
5. **Customer uses tokens** to access the service (token balance checked per request)

### MVC Implementation

```
app/
  models/
    product.h       — name, description, price_zcl, token_id
    order.h         — customer_addr, amount, payment_txid, status
    token_balance.h — address, token_id, balance

  controllers/
    store_controller.h
      GET  /store              → list products
      GET  /store/product/:id  → product detail + payment address
      POST /store/buy/:id      → generate z-addr, show QR/address
      GET  /store/status/:txid → check payment, mint tokens if paid
      GET  /store/account      → show token balance

  views/
    store/
      index.html       — product listing
      product.html     — buy page with z-address
      status.html      — payment confirmation
      account.html     — token balance dashboard
```

### Payment Flow
```
Customer                     Merchant Node
   |                              |
   |  GET /store                  |
   |<──── product listing ────────|
   |                              |
   |  GET /store/product/1        |
   |<──── z-addr for payment ─────|
   |                              |
   |  z_sendmany to z-addr        |
   |  (shielded, private)         |
   |────── ZCL payment ──────────>|
   |                              |
   |  GET /store/status/:txid     |
   |<──── "confirmed, minting" ───|
   |                              |
   |                   ZSLP SEND  |
   |<──── tokens to t-addr ───────|
   |                              |
   |  GET /store/account          |
   |<──── balance: 100 tokens ────|
```

### Why Shielded Payments Matter
- Merchant can't see customer's total balance
- Third parties can't see payment amounts
- No payment processor taking 3% cut
- No chargebacks (blockchain finality)
- No KYC/AML friction
- Works globally, instantly, permissionlessly

### Token-Gated Access
Every request to a protected route checks the customer's ZSLP balance:

```c
// In controller before_action hook
bool require_tokens(const char *customer_addr, const char *token_id, uint64_t amount) {
    uint64_t balance = zslp_balance_of(customer_addr, token_id);
    if (balance < amount) {
        // Redirect to /store with "insufficient tokens" message
        return false;
    }
    // Debit tokens (ZSLP SEND back to merchant, burning customer's balance)
    zslp_transfer(customer_addr, merchant_addr, token_id, amount);
    return true;
}
```

## Backend Requirements

### P2P (Clearnet — Blockchain Only)
- [x] Bug-for-bug zclassicd compatible (NODE_NETWORK | NODE_BLOOM)
- [x] Fast sync with PoW defense (1.6M UTXOs in 60s)
- [x] Rate limiting (5000 chunks/IP/hour)
- [ ] Peer scoring + addr relay
- [ ] Block + transaction relay validation

### Tor (All Application Traffic)
- [x] Embedded in binary (SocksPort 0, no ports)
- [x] .onion address generation
- [ ] dynhost → onion_service_handle_request wired
- [ ] Persistent .onion key
- [ ] .onion published on-chain via ZSLP

### On-Chain Registry
- [x] ZSLP token framework (GENESIS, MINT, SEND, BURN)
- [x] .onion hostname in OP_RETURN
- [x] Chain scan for peer discovery
- [ ] Site metadata (title, description, category)
- [ ] Load balancer replica announcements
- [ ] Content version anchoring

### Commerce
- [x] Product model (SQLite, auto-schema)
- [x] Order model with payment state machine (pending/paid/minted)
- [x] Store controller: list/detail/buy/status routes
- [x] Demo products seeded on first access (3 products)
- [x] z-address generation per order
- [x] ZSLP token controller (create/mint/send/balance)
- [x] Token balance tracking (zslp_balances table)
- [ ] Payment detection loop (z_listunspent polling)
- [ ] Auto-mint tokens on payment confirmation
- [ ] Token-gated route access (before_action check)

### Load Balancing (10 tests passing)
- [x] Replica discovery from ZSLP chain scan
- [x] Scoring: reachable > latency > capacity > freshness
- [x] Capacity-aware selection
- [x] Failover (site_connect_best tries multiple replicas)
- [x] Content version consistency field
- [x] Replica announcement (site_announce_replica builds ZSLP script)
- [ ] Actual Tor circuit probing (currently estimates from block height)
- [ ] Content sync between replicas

### P2P Gaming / Low-Latency (15 tests passing)
- [x] Tic-tac-toe engine (move validation, win/draw detection)
- [x] Wire format: zgame command (invite/accept/move/state/result)
- [x] Microsecond latency measurement per move
- [x] Auto-accept game invites
- [x] Board rendering
- [ ] ZSLP token staking (winner gets pot)
- [ ] Game lobby (discover opponents via P2P)
- [ ] Chess / custom game types

### MVC Framework
- [x] Models: SQLite with ActiveRecord pattern
- [x] Controllers: route handlers with RPC dispatch
- [x] Views: HTML served from files, inline CSS
- [x] Form data parsing (POST body → key/value)
- [ ] HTML template engine ({{variable}} substitution)
- [ ] Before/after action hooks
- [ ] Session state (per-Tor-circuit)
- [ ] CSRF protection (per-circuit tokens)

## The Growth Cycle

```
1. Run ./zclassic23 -tor
2. Fast sync UTXO set (~60s)
3. Node becomes full zclassicd peer (clearnet blockchain)
4. Node creates .onion (Tor application layer)
5. Build your store/blog/app (MVC in C23)
6. Accept shielded ZCL payments
7. Issue ZSLP tokens to customers
8. Publish .onion on-chain for discovery
9. Scale with replicas (multiple nodes, one address via ZSLP)
10. The network grows — every operator is also a full node
```

## What Makes This Different

| Traditional Web | ZClassic23 |
|----------------|------------|
| Server + domain + TLS | Single binary, .onion auto-generated |
| External payment API | Native shielded transactions |
| Database + app server | SQLite + MVC in one process |
| DNS resolution | On-chain ZSLP lookup |
| Load balancer appliance | On-chain replica discovery |

## License

Copyright 2026 Rhett Creighton — Apache License 2.0
