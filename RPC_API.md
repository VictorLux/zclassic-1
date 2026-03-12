# ZClassic C23 Node — RPC API Reference

## Wallet RPCs

### getnewaddress
Generate a new transparent (t-addr) address.
```
getnewaddress
→ "t1..."
```

### getbalance
Get total confirmed wallet balance.
```
getbalance
→ 1.00000000
```

### getunconfirmedbalance
Get total unconfirmed (mempool) balance.
```
getunconfirmedbalance
→ 0.0001
```

### listunspent
List unspent transaction outputs (UTXOs).
```
listunspent
→ [{"txid":"...", "vout":0, "address":"t1...", "amount":0.1, "confirmations":1, "spendable":true, "solvable":true}]
```

### sendtoaddress
Send ZCL to a single address. Returns txid.
```
sendtoaddress "t1..." 0.0001
→ "txid_hex"
```

### sendmany
Send to multiple addresses in one transaction. First arg must be "".
```
sendmany "" {"t1abc...":0.1, "t1def...":0.1, "t1ghi...":0.1}
→ "txid_hex"
```

### createmultisig
Create a multisig P2SH address from public keys (does NOT add to wallet).
```
createmultisig 2 ["pubkey1_hex", "pubkey2_hex", "pubkey3_hex"]
→ {"address":"t3...", "redeemScript":"hex"}
```

### addmultisigaddress
Create a multisig P2SH address AND store the redeem script in the wallet.
Required for spending from multisig addresses.
```
addmultisigaddress 2 ["pubkey1_hex", "pubkey2_hex", "pubkey3_hex"]
→ {"address":"t3...", "redeemScript":"hex"}
```

### validateaddress
Return information about a ZClassic address.
```
validateaddress "t1..."
→ {"address":"t1...", "isvalid":true, "ismine":true, "isscript":false, "pubkey":"hex", "iscompressed":true}
validateaddress "t3..."
→ {"address":"t3...", "isvalid":true, "ismine":true, "isscript":true}
```

### dumpprivkey
Export private key for a transparent address (WIF format).
```
dumpprivkey "t1..."
→ "L4bGRKw1..."
```

### importprivkey
Import a private key. Optional: label, rescan flag, start height.
```
importprivkey "L4bGRKw1..." "" false 3035845
```

### keypoolrefill
Refill the keypool with new pre-generated keys.
```
keypoolrefill 100
```

### rescanblockchain
Rescan blockchain for wallet transactions in a block range.
```
rescanblockchain 3035845 3036000
→ {"start_height": 3035845, "stop_height": 3036000}
```

### listtransactions
List recent wallet transactions.
```
listtransactions
→ [{"txid":"...", "amount":0.1, "confirmations":5, ...}]
```

### gettransaction
Get details of a wallet transaction.
```
gettransaction "txid_hex"
→ {"amount":..., "confirmations":..., "hex":"..."}
```

### getwalletinfo
Get wallet metadata.
```
getwalletinfo
→ {"walletversion":..., "balance":..., "keypoolsize":...}
```

## Shielded (Sapling) RPCs

### z_getnewaddress
Generate a new Sapling shielded address (zs1...).
Uses ZIP-32 HD derivation: m/32'/147'/account'.
Keys are persisted to wallet DB across restarts.
```
z_getnewaddress
→ "zs1..."
```

### z_listaddresses
List all Sapling addresses in the wallet.
```
z_listaddresses
→ ["zs1...", "zs1..."]
```

## Blockchain RPCs

### getblockcount
```
getblockcount → 3036031
```

### getbestblockhash
```
getbestblockhash → "0000..."
```

### getblockhash
```
getblockhash 3036031 → "0000..."
```

### getblock
```
getblock "hash" [verbosity]
```

### getblockheader
```
getblockheader "hash" [verbose]
```

### getblockchaininfo
```
getblockchaininfo → {"chain":"main", "blocks":..., ...}
```

### getdifficulty
```
getdifficulty → 12345.678
```

### getmempoolinfo
```
getmempoolinfo → {"size":5, "bytes":1250}
```

## Raw Transaction RPCs

### getrawtransaction
Three-tier lookup: mempool → txindex → coins DB.
```
getrawtransaction "txid" [verbose]
```

### decoderawtransaction
```
decoderawtransaction "hex"
```

### sendrawtransaction
```
sendrawtransaction "hex"
```

### createrawtransaction
```
createrawtransaction [{"txid":"...","vout":0}] {"address":amount}
```

## Network RPCs

### getnetworkinfo
```
getnetworkinfo → {"version":..., "subversion":"...", "connections":8}
```

### getpeerinfo
```
getpeerinfo → [{"addr":"...", "services":"...", ...}]
```

### getconnectioncount
```
getconnectioncount → 8
```

### ping / addnode
```
ping
addnode "ip:port" "add"
```

## Mining RPCs

### getmininginfo / getblocksubsidy / getblocktemplate / submitblock

## Misc RPCs

### getinfo / stop

## Address Types

| Type | Prefix | Example | Description |
|------|--------|---------|-------------|
| P2PKH | t1 | t1TgQiuMo... | Standard transparent |
| P2SH | t3 | t3Vz7g... | Pay-to-script-hash (multisig) |
| Sapling | zs1 | zs1mmejk... | Shielded (z-addr) |

## Transaction Types Supported

- **t→t**: Transparent to transparent (fully supported, tested)
- **t→z**: Transparent to shielded (requires Groth16 prover — not yet implemented)
- **z→t**: Shielded to transparent (requires Groth16 prover — not yet implemented)
- **z→z**: Shielded to shielded (requires Groth16 prover — not yet implemented)
- **Multisig**: P2SH multisig creation and wallet storage (createmultisig, addmultisigaddress — implemented)

## Build & Run

```bash
cd src/c23 && make zcld    # Build node
cd src/c23 && make test     # Run unit tests

# Run C23 node
./zcld -datadir=~/.zclassic-c23 -port=18033 -rpcport=18232 \
  -rpcuser=c23user -rpcpassword=c23pass -txindex -showmetrics=0

# RPC calls via C++ CLI
zclassic-cli -rpcuser=c23user -rpcpassword=c23pass -rpcport=18232 <command>
```
