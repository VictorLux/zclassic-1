# Wave 30 Task 3: Debug and fix Zelcore peer connection rejection

## Problem

Our node (205.209.104.118:8033) cannot connect to any Zelcore network peers. TCP connects succeed but the remote immediately RSTs us after receiving our version message. The local C++ zclassicd on the same machine (74.50.74.102:8034) connects to these same peers fine.

## Zelcore Peers (from explorer.zcl.zelcore.io/network)

| IP | Port | Subversion | Status from us |
|----|------|-----------|---------------|
| 37.187.76.79 | 8033 | MagicBean:2.1.19 | TCP open, RST after version |
| 162.55.92.62 | 8033 | MagicBean:2.1.110 | Connection refused |
| 157.90.223.151 | 8033 | MagicBean:2.1.110 | Connection refused |
| 140.174.189.17 | 8033 | MagicBean:2.1.110 | TCP open, RST after version |
| 157.173.195.203 | 8033 | MagicBean:2.1.2beta1 | TCP open, RST after version |
| 85.239.232.93 | 8033 | MagicBean:2.1.2beta1 | TCP open, RST after version |
| 154.38.178.121 | 8033 | MagicBean:2.1.2beta1 | TCP open, RST after version |
| 51.178.179.75 | 8033 | MagicBean:2.1.110 | TCP open, RST after version |

## What We Ruled Out

- **User agent filtering** — tested with `/MagicBean:2.1.1-10/` UA, still RST
- **Version message format** — raw Python version messages with correct magic (24e92764), proto 170011, correct payload size (106 bytes) also get RST
- **Local C++ node accepts our format** — same Python script connecting to 127.0.0.1:8034 gets a valid version response
- **Protocol version** — we send 170011 which matches BUTTERCUP epoch requirement

## Key Observation

Our node doesn't know its external IP (`localaddresses: []` in getnetworkinfo). It connects to local zclassicd from 127.0.0.1, so the C++ node can't relay our address to the network.

The C++ zclassicd at 74.50.74.102:8034 IS connected to these Zelcore peers. Its `getpeerinfo` shows connections to 5 of them.

## Investigation Steps

1. **Test if it's IP-based rejection**: Use the C++ node's RPC to make a Zelcore peer connect to us:
   ```bash
   # C++ node RPC (port 8232, cookie auth at ~/.zclassic/.cookie)
   curl -s --user "$(cat ~/.zclassic/.cookie)" \
     --data-binary '{"method":"addnode","params":["205.209.104.118:8033","onetry"]}' \
     -H 'content-type:text/plain;' http://127.0.0.1:8232/
   ```

2. **Check if our addr propagates**: After implementing -externalip (Task 1), check if the C++ node relays our address

3. **Check maxconnections**: The Zelcore nodes might have maxconnections=8 or similar. If they already have 8 peers they won't accept new inbound. Check if any of them have free connection slots.

4. **Try the newest peer**: 51.178.179.75 has only 15hrs uptime — it might have fewer connections and accept us

5. **Check for IP banning**: Maybe our IP got temp-banned from repeated connection attempts. The C++ code has `ClearBanned()` RPC — check if we're banned on any of them (we can't check directly, but we can check if waiting helps)

## Our Node Details

- IP: 205.209.104.118
- P2P port: 8033
- Protocol: 170011
- User agent: /ZClassic-C23:1.0.0/
- Magic bytes: 24 e9 27 64
- Onion: oaejwtr7wd6ah6csxz4vy4iro6l5cxc2flbmxkhgybgafuu25fg7nkid.onion

## C++ zclassicd Details

- IP: 74.50.74.102
- P2P port: 8034
- RPC port: 8232
- Cookie: ~/.zclassic/.cookie
- User agent: /MagicBean:2.1.1-10/

## Deliverables

- Document what you find (update this file with findings)
- If you find a fix, implement it
- If it's maxconnections, propose a strategy (e.g., periodic connection attempts with backoff)
