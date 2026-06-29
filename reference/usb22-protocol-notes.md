# USB22-485 — Reverse-Engineered USB Protocol Notes

Source device observed: **VID 0x0D0B / PID 0x1002** ("USB2.0 BACKPLANE MODE ARCNET ADAPTER",
firmware-loaded stage). Captured with Wireshark/USBPcap while the original CC driver +
Talk.exe were running on Windows 11. Chip: COM20022 ARCNET controller behind an
EZ-USB-class microcontroller.

## Status: DECODED so far
- [x] Endpoint architecture
- [x] Register read/write command + response framing
- [x] Unsolicited status/event message framing
- [ ] ARCNET packet transmit (TODO — next capture)
- [ ] ARCNET packet receive (TODO — next capture)
- [ ] Init register sequence semantics (partially captured)
- [ ] Firmware download / B001->1002 transition (TODO — separate capture)

## Endpoints
| Endpoint | Direction | Type | Use |
|----------|-----------|------|-----|
| 0x01 | OUT (host->dev) | Bulk | Commands |
| 0x81 | IN  (dev->host) | Bulk | Responses + unsolicited events |
| 0x02 / 0x86 | OUT/IN | Bulk (512-byte) | Reported in descriptor; likely bulk ARCNET data path (unconfirmed) |
| 0x00 | control | Control | Standard USB enumeration/descriptors |

## Command: Register access (opcode 0x01)
Host -> device, EP 0x01 OUT, **5 bytes**:
```
byte0 = 0x01            ; opcode: register access
byte1 = 0x00            ; reserved / board number (always 0x00 observed)
byte2 = bWrite          ; 0x00 = read, 0x01 = write
byte3 = byRegister      ; COM20022 register number (0..7)
byte4 = byValue         ; value to write (0x00 on read)
```

Device -> host, EP 0x81 IN, **7 bytes**:
```
byte0     = 0x01        ; opcode echo
byte1..3  = 0x00 0x00 0x00 ; reserved / status
byte4     = bWrite echo
byte5     = byRegister echo
byte6     = byValue     ; READ: value returned by chip / WRITE: echo of written value
```

Maps to ArcX.h `COM20020_REGISTER { UCHAR bWrite; UCHAR byRegister; UCHAR byValue; }`
with a 2-byte command prefix (`01 00`) prepended.

### Verified examples
| Host OUT (5B) | Meaning | Device IN (7B) | Result |
|---------------|---------|----------------|--------|
| 01 00 01 00 03 | write reg0 = 0x03 | 01 00 00 00 01 00 03 | write echoed/ack |
| 01 00 01 02 04 | write reg2 = 0x04 | 01 00 00 00 01 02 04 | write echoed/ack |
| 01 00 01 01 16 | write reg1 = 0x16 | 01 00 00 00 01 01 16 | write echoed/ack |
| 01 00 01 02 c0 | write reg2 = 0xC0 | 01 00 00 00 01 02 c0 | write echoed/ack |
| 01 00 01 00 08 | write reg0 = 0x08 | 01 00 00 00 01 00 08 | write echoed/ack |
| 01 00 00 02 00 | read reg2 | 01 00 00 00 00 02 c0 | reg2 = 0xC0 |
| 01 00 00 02 00 | read reg2 | 01 00 00 00 00 02 00 | reg2 = 0x00 |
| 01 00 00 01 00 | read reg1 | 01 00 00 00 00 01 20 | reg1 = 0x20 |
| 01 00 00 01 00 | read reg1 | 01 00 00 00 00 01 a0 | reg1 = 0xA0 |

Pairing confirmed in capture: every EP0x01 OUT is immediately followed by one
EP0x81 IN response (request/response, strictly serialized).

## Unsolicited status/event message (opcode 0x20)
Device -> host, EP 0x81 IN, **6 bytes**, sent without a preceding command:
```
20 00 00 00 04 00
byte0 = 0x20    ; opcode: status/event notification
byte5/byte4 region carries status/flag bits (0x04 observed)
```
Streamed continuously during the capture (84 occurrences). Corresponds to the
async notification channel behind Com20020WakeOnRecon / WakeOnReceive /
WakeOnTXComplete. With no ARCNET peer attached, the device keeps reporting
RECON-type status, which is why Talk's "Number of RECONs" kept rising.

## Open questions for the next capture
1. Transmit path: what opcode does `Com20020Transmit` produce, and does the 508-byte
   payload go over EP 0x01 or the 512-byte EP 0x02?
2. Receive path: how are inbound ARCNET packets delivered — polled via opcode, or
   pushed on EP 0x81 / EP 0x86?
3. The exact init register-write sequence (order + values) that `Com20020Init` performs,
   so we can replicate chip configuration.

---

# UPDATE 2 — Init + Transmit decoded (capture 02-transmit.pcapng, dev addr 15, PID 0x1002)

## EP 0x01 command framing (generalized)
General form on EP 0x01 OUT: `[opcode] [0x00] [args...]`, response on EP 0x81 IN.

| Opcode | Name | Args | Notes |
|--------|------|------|-------|
| 0x00 | Init | 10-byte COM20020_CONFIG | configures the chip |
| 0x01 | Register access | 3-byte COM20020_REGISTER | read/write reg 0..7 |
| 0x04 | (unknown, no args) | none (`04 00`) | seen once near TX; maybe FlushRX/CancelTX/arm — TBD |

## Init command (opcode 0x00) — DECODED
Observed OUT: `00 00 00 00 00 18 01 01 00 00 00 01`
= opcode `00 00` + COM20020_CONFIG (10 bytes, pack(1)):
```
uiCom20020BaseIOAddress  = 0x0000   (USHORT LE)
byCom20020InterruptLevel = 0x00
byCom20020Timeout        = 0x18     (STANDARD_TIMEOUT)
byCom20020NodeID         = 0x01     (node 1, as entered in Talk)
bCom20020_128NAKs        = 0x01     (TRUE)
bCom20020ReceiveAll      = 0x00     (FALSE)
byCom20020ClockPrescaler = 0x00     (SPEED_2500KHZ -> 2.5 Mbps, as chosen)
bCom20020SlowArbitration = 0x00     (FALSE)
bCom20020ReceiveBroadcasts = 0x01   (TRUE)
```
Every field matches the values entered in Talk's connect dialog -> confirmed.

## Transmit (EP 0x02 OUT, bulk) — DECODED
Observed OUT: `02 f7 44 45 41 44 31 32 33 34 35`  ("DEAD12345" sent to node 2)
```
byte0      = destination node ID        (0x02 = node 2, as entered)
byte1      = ARCNET count = 256 - L      (0xF7 = 247 = 256 - 9; L = 9 data bytes)
byte2..    = data payload (L bytes)
```
Source node ID is inserted by the device firmware (from Init config), not sent by host.
Transmit uses the dedicated 512-byte bulk endpoint EP 0x02, NOT the EP 0x01 command channel.

## Receive (EP 0x02 OUT request / EP 0x86 IN, bulk) — PARTIAL
A 10-byte all-zero exchange was seen: OUT `00..00` (10B) on EP 0x02, IN `00..00` (10B)
on EP 0x86. Likely a receive poll returning empty (no ARCNET peer attached, so no packet).
Needs a capture WITH an inbound packet (second node / peer) to fully decode the inbound
framing and the dwNumberOfFilledBuffers semantics.

## Remaining TODO
- [ ] Decode opcode 0x04 (no-arg command)
- [ ] Capture a real inbound packet to decode receive framing
- [ ] Capture the full Init register-write sequence that follows opcode 0x00
      (the chip-level reg writes the firmware performs, if any are visible)
- [ ] Firmware download / B001->1002 transition (separate capture)

---

# UPDATE 3 — Receive decoded (capture 03-receive.pcapng; two devices: addr22=node1, addr24=node2)

## Receive (EP 0x86 IN, bulk) — DECODED
Inbound ARCNET packets are delivered on EP 0x86 IN with format:
```
byte0   = source node ID
byte1   = destination node ID
byte2   = ARCNET count = 256 - L
byte3.. = data (L bytes)
```
Maps to ArcX.h COM20020_RECEIVE_BUFFER { bySourceNodeID; byDestinationNodeID;
USHORT uiNumberOfBytes; ... } — the DLL derives uiNumberOfBytes = 256 - count.

### Verified (both directions, same capture)
| Sender | On wire (EP0x02 OUT) | Receiver gets (EP0x86 IN) | Data | L | count |
|--------|----------------------|----------------------------|------|---|-------|
| node1->node2 | 02 f7 + "BEEF67890" | 01 02 f7 + "BEEF67890" | BEEF67890 | 9 | 0xF7=256-9 |
| node2->node1 | 01 f8 + "DEAD3131"  | 02 01 f8 + "DEAD3131"  | DEAD3131  | 8 | 0xF8=256-8 |

Transmit (EP0x02 OUT) = [dst][count][data]; Receive (EP0x86 IN) = [src][dst][count][data].
Source node on TX is inserted by firmware; on RX it is reported in byte0.

## CORE PROTOCOL NOW COMPLETE
- Init        : EP0x01 OUT  `00 00` + COM20020_CONFIG(10)        -> resp EP0x81
- Register    : EP0x01 OUT  `01 00` + {bWrite,reg,val}           -> resp EP0x81 (7B)
- Transmit    : EP0x02 OUT  [dst][256-L][data]
- Receive     : EP0x86 IN   [src][dst][256-L][data]
- Async event : EP0x81 IN   `20 00 00 00 04 00` (status/RECON notifications)

## Still to confirm during coding (minor)
- Receive request/poll handshake: capture 2 showed a 10-byte all-zero OUT on EP0x02
  followed by 10-byte all-zero IN on EP0x86 (empty poll). Confirm whether host must
  issue a poll request per receive, or simply keep a blocking read posted on EP0x86.
- Opcode 0x04 (no-arg `04 00`) semantics.

---

# UPDATE 4 — Response framing & event interleaving (critical for reading EP 0x81)

## EP 0x81 IN carries BOTH command responses AND unsolicited events, interleaved
The device continuously pushes unsolicited event messages on EP 0x81. A command's
response can therefore be preceded/followed by event messages on the same pipe.
**When reading a command response, filter by opcode (byte0):**
- Event message  : byte0 = 0x20  -> not a command response; skip (or handle separately)
- Command response: byte0 = echo of the command opcode you sent (0x00/0x01/0x04/...)
Read in a loop on EP 0x81 until byte0 == the opcode you sent.

## Response lengths are VARIABLE per opcode (do NOT read a fixed 7)
Observed (capture 02-transmit, dev addr 15):
| Command sent (EP0x01 OUT) | Response (EP0x81 IN) | Len |
|---------------------------|----------------------|-----|
| `04 00` (opcode 0x04, no args) | `04 00 00 00` | 4 |
| `00 00 ..CONFIG..` (opcode 0x00 init) | `00 00 00 00 22 00` | 6 |
| `01 00 bWrite reg val` (opcode 0x01) | `01 00 00 00 bWrite reg val` | 7 |
| (unsolicited) | `20 00 00 00 04 00` | 6 |

Recommendation: post a read with a generous buffer (e.g. 64 bytes) and use the actual
bytesTransferred; parse by opcode. Init response status byte (here 0x22) = init status;
treat non-error as success.

## Opcode 0x04 (no-arg) — observed at startup, before init
`04 00` -> `04 00 00 00`. Issued once at session start. Likely a reset/flush/"begin
session" command. Replicating it before init is harmless and matches the original driver;
recommend sending `04 00` first, then init.

## Confirmed live with our own WinUSB code (post-Zadig)
- arc_open + WinUsb_Initialize: OK (VID 0D0B/PID 1002, iface class 0xFF, 4 bulk EPs)
- arc_init: command bytes byte-identical to captured init; chip configured (proven below)
- register reads returned reg1=0xA0, reg2=0xC0, reg0=0x24 — consistent with capture.

---

# UPDATE 5 — REQUIRED startup handshake before init (root cause of init silence)

Byte-level diff of the original driver (Talk) vs our code showed our init got NO response
because we skipped a mandatory data-channel handshake. The original driver's startup is:

```
1. EP0x01 OUT: 04 00                          ; cmd 0x04 (begin session)
   EP0x81 IN : 04 00 00 00                     ; response
2. EP0x02 OUT: 00 00 00 00 00 00 00 00 00 00   ; 10-byte all-zero  <-- REQUIRED, easy to miss
   EP0x86 IN : 00 00 00 00 00 00 00 00 00 00   ; 10-byte response (all-zero when idle)
3. EP0x01 OUT: 00 00 + COM20020_CONFIG(10)     ; init
   EP0x81 IN : 00 00 00 00 22 00               ; init response (status byte, here 0x22)
   EP0x81 IN : 20 00 00 00 04 00 ...           ; event stream begins
```

Without step 2, the device does NOT respond to init (EP0x81 stays empty) and the event
stream never starts. Step 2 is a data-channel (EP0x02/EP0x86) probe/handshake that also
doubles as the receive-poll. For startup, perform: write 10 zero bytes to EP0x02, then
read EP0x86 (expect 10 bytes; all-zero when no RX pending). Then send init.

Likely also used as the RX poll during operation: write a 10-byte request on EP0x02,
read EP0x86; a received packet comes back as [src][dst][256-L][data] (see UPDATE 3),
all-zero when nothing is pending.

---

# UPDATE 6 — ROOT CAUSE of init "no response": init is SLOW (~2.5 s latency)

Timing diff (captures):
- Talk: init OUT @ t=31.711s  ->  init response (00 00 00 00 22 00) @ t=34.214s
  => device takes ~2.5 SECONDS to answer init.
- Our code: init OUT @ t=12.122s, read EP0x81 @12.122 (0 bytes), again @13.122 (0 bytes),
  then gave up ~1s after init. We quit BEFORE the response was due.

Why slow: init configures the COM20022 and waits for the ARCNET network/token to settle
(or a fixed RECON/timeout) before returning status. Register reads are instant; init is not.
While "thinking", the device returns zero-length IN packets (ZLP) on EP0x81 — these mean
"not ready yet", NOT "done" and NOT an error.

## Fix
- init must use a LONG response wait: total budget >= ~4 s (use ~5 s to be safe).
- Read EP0x81 in a loop: a 0-byte read = keep waiting (don't count as a failed attempt);
  skip 0x20 events; succeed when byte0 == 0x00 (init opcode echo); response = 6 bytes.
- Other commands (register/cmd04) stay fast (~1 s is fine). Only init needs the long wait.

---

# UPDATE 7 — END-TO-END VERIFIED with our own WinUSB code (no CC driver)

All four core operations confirmed working through our user-mode WinUSB layer:
- Init     : OK (response 00 00 00 00 22 00, status 0x22)
- Register : OK (reg reads consistent with capture)
- Transmit : OK — sent "DEAD12345" to node 2 from our code; received by Talk (independent device)
- Receive  : OK — Talk (node 2) sent "CAFE9999" to node 1; our code read it on EP0x86:
             src=2, dst=1, len=8, "CAFE9999"

Confirmed startup sequence (the working recipe):
  1. open device by path (WinUsb_Initialize)
  2. cmd04:      EP0x01 OUT `04 00`            -> EP0x81 IN `04 00 00 00`
  3. handshake:  EP0x02 OUT 10x00              -> EP0x86 IN 10x00
  4. init:       EP0x01 OUT `00 00`+CONFIG(10) -> EP0x81 IN 6B (WAIT up to ~5s; status byte)
  5. transmit:   EP0x02 OUT [dst][256-L][data]
  6. receive:    EP0x02 OUT 10x00 (poll) -> EP0x86 IN [src][dst][256-L][data] (00.. if none)

Notes for production:
- init is slow (~2.5s); use a long response wait. Other commands are fast.
- EP0x81 interleaves command responses (opcode echo) with async events (opcode 0x20);
  filter by byte0 / loop until the expected opcode.
- Two devices share VID/PID; select by device path. Driver binding here turned out to be
  per-port (one USB port bound to WinUSB, another left on the CC driver).
- ARCNET node IDs must be unique on the bus; mismatched/duplicate node IDs cause init to
  report a non-0x22 status (e.g. 0xFB) and the network not to settle.

---

# UPDATE 8 — ACK detection (transmit delivery confirmation)

ARCNET hardware ACK is reported in the COM20022 STATUS register (register 0), bit 1 = TMA
("Transmit Message Acknowledged"). Read register 0 right AFTER arc_transmit:

| Scenario (node1 -> node2, "ACKTEST1") | Reg[0] after TX | bit1 (TMA) | Meaning |
|---------------------------------------|-----------------|-----------|---------|
| receiver ALIVE (node2 open+init, bus wired) | 0x27 = 0010 0111 | 1 | delivered / ACKed |
| receiver DEAD (node2 absent)                | 0x00 = 0000 0000 | 0 | not ACKed |

Reg[0] bit layout observed: bit0=TA (Transmitter Available), bit1=TMA (ACK), bit2=EST,
bit3=RI/recon-ish, bit5=POR. The reliable delivery indicator is **bit1 (TMA = 0x02)**.

Secondary signal: when receiver is DEAD, the device also emits a `20 00 00 00 04 00`
event on EP0x81 (RECON/no-token); when ALIVE, no such event. The register bit is the
cleaner check.

## Implementation
After arc_transmit succeeds (bytes written to EP0x02), read register 0 (arc_register read
reg 0). If (reg0 & 0x02) -> delivered (ARC_OK / ARC_ACKED); else -> not delivered
(ARC_NOT_ACKED). Note: there may be a short delay before TMA settles; read reg0 a few ms
after the write, and optionally poll reg0 a few times (e.g. up to ~50-100 ms) for the
TMA bit before concluding NOT_ACKED.

---

# UPDATE 9 — Long-frame (>253 byte) transmit format (capture 14-big_datas.pcapng)

Source: Wireshark/USBPcap capture of CC driver + Talk.exe transmitting payloads larger
than 253 bytes. Captured on EP 0x02 OUT bulk transfers.

## Short frame vs Long frame on EP 0x02 OUT

### Short frame (payload 1..253 bytes) — confirmed in UPDATE 2 / 3
```
EP0x02 OUT: [dst] [256-L] [data × L]
  USB transfer total: 2 + L bytes
  byte0 = destination node ID
  byte1 = ARCNET count = 256 - L    (ranges 3..255; NEVER 0x00)
```
Verified:
- L=252 → byte1 = 0x04 (256−252 = 4)
- L=253 → byte1 = 0x03 (256−253 = 3)  ← maximum short-frame payload

**byte1 is NEVER 0x00 for short frames** (minimum count = 256−253 = 3).

### Long frame (payload 254..508 bytes) — NEW
```
EP0x02 OUT: [dst] [0x00] [512-L] [data × L]
  USB transfer total: 3 + L bytes
  byte0 = destination node ID
  byte1 = 0x00                    (long-frame marker; impossible in short frame)
  byte2 = 512 - L                 (effective range 4..255 for L=257..508)
```
Disambiguation: byte1 == 0x00 is impossible in a short frame (count ≥ 3), so it
unambiguously signals long frame on both transmit (EP0x02 OUT) and receive (EP0x86 IN,
unverified — see below).

Verified in capture:
- L=370 → header bytes: `[dst] 00 8E`  (0x8E = 142 = 512−370); USB transfer = 373 bytes

Maximum payload: 508 bytes (byte2 minimum = 512−508 = 4; values below 4 are outside
ARCNET framing constraints).

Note on boundary range (L=254..256): 512−L yields 258, 257, 256 respectively, which
exceed one byte. These values are NOT observed in capture; behaviour at the boundary is
UNVERIFIED. Safe implementation: start long-frame format at L=254 with `(512-L) & 0xFF`
and avoid payloads in this boundary range until confirmed.

## Long-frame receive format — CONFIRMED (same capture)
Receive-side (EP 0x86 IN) long-frame format mirrors the transmit side with an
extra leading `src` byte (same as short-frame receive):

```
EP0x86 IN short: [src][dst][256-L][data × L]        header = 3 bytes
EP0x86 IN long:  [src][dst][0x00][512-L][data × L]  header = 4 bytes
```

The disambiguation rule is identical: the "length byte" (byte2 for receive) is
`0x00` for long frames and always ≥ 3 for short frames (`256-L`, L ≤ 253).

Verified in capture (same 370-byte transfer, receive side):
- buf = `[src][dst] 00 8E [370 bytes]`  (0x8E = 142 = 512−370) ✓
- Total bytes received from EP 0x86: 4 (header) + 370 (payload) = 374 bytes

## Summary: frame format by direction
| Direction | Format | Payload L | Header bytes | EP bytes total |
|-----------|--------|-----------|--------------|----------------|
| TX EP0x02 OUT | Short | 1..253 | `[dst][256-L]` (2 B) | 2+L |
| TX EP0x02 OUT | Long  | 254..508 | `[dst][0x00][512-L]` (3 B) | 3+L |
| RX EP0x86 IN  | Short | 1..253 | `[src][dst][256-L]` (3 B) | 3+L |
| RX EP0x86 IN  | Long  | 254..508 | `[src][dst][0x00][512-L]` (4 B) | 4+L |
| Both | Invalid | > 508 | — | ARC_ERR_PARAM / drop |

---

# UPDATE 10 — TX-complete ACK event on EP0x81 (test_txevent capture)

## Discovery

reg0/TMA polling (UPDATE 8) is fundamentally unreliable: TMA is a momentary pulse
cleared by token-passing (~1–2 ms), while the arc_register round-trip (two USB
transfers on EP0x01/EP0x81) takes 2–6 ms — a race that is lost most of the time.
Observed symptom: art arda TX'lerde ilk 1-2 ARC_OK sonra sürekli ARC_NOT_ACKED.

Root cause confirmed via test_txevent: the data IS delivered (node2 receives all
frames), but TMA is gone by the time we read reg0.

## TX-complete event (opcode 0x20) on EP0x81

After `WinUsb_WritePipe(EP0x02, frame)` succeeds, the device firmware **pushes a
0x20 event on EP0x81** to report the transmit outcome.  This is the same event
channel used for RECON (UPDATE 1: WakeOnRecon / WakeOnReceive / **WakeOnTXComplete**).

Observed event format (6 bytes):
```
byte0 = 0x20   (opcode: async event)
byte1 = 0x00
byte2 = 0x00
byte3 = 0x00
byte4 = XX     ← TX outcome indicator
byte5 = 0x00
```

### byte4 (b4) bit structure — VERIFIED (test_b4survey, 3 scenarios, 20 TX each)

b4 encodes two independent fields:

```
bit0 (0x01): TX-complete flag
             1 = this event belongs to our TX (act on it)
             0 = saf RECON / other network event (skip, keep waiting)

bit1 (0x02): ACK flag  (valid only when bit0 = 1)
             1 = ACK received from destination
             0 = no ACK (destination absent or didn't respond)

bit2 (0x04): RECON side-info
             1 = network reconfiguration occurred alongside this TX
             does NOT affect ACK decision
```

Verified combinations:

| b4   | bin | bit0 | bit1 | bit2 | Scenario | Meaning |
|------|-----|------|------|------|----------|---------|
| 0x01 | 001 | 1    | 0    | 0    | node→nonexistent | TX-complete, no ACK |
| 0x03 | 011 | 1    | 1    | 0    | node→alive      | TX-complete, ACK |
| 0x04 | 100 | 0    | 0    | 1    | standalone RECON | not TX-complete → skip |
| 0x05 | 101 | 1    | 0    | 1    | TX + RECON      | TX-complete, no ACK |
| 0x07 | 111 | 1    | 1    | 1    | TX + ACK + RECON | TX-complete, ACK |

### 3-scenario evidence (test_b4survey, bit1 distribution)

| Scenario | Description | bit1 set / 20 TX |
|----------|-------------|-----------------|
| A — ALIVE       | node1 → node2 (open+init, chip on bus) | **20/20** |
| B — NO-RECEIVER | node1 → node5 (non-existent)           | **0/20**  |
| C — CHIP-ALIVE  | node1 → node2 (arc_close'd, chip still on bus, HW ACK reflex) | **20/20** |

bit1 is the clean ACK discriminator regardless of bit2 or other flags.

Note: arc_close does NOT pull the chip off the ARCNET bus — the COM20022 chip
continues to respond with hardware ACK (bit1=1) even after the host software closes
the USB handle.  Only the data path (EP0x86 receive) stops.

## Implementation (updated — UPDATE 11)

`arc_transmit(waitAck=true)` reads EP0x81 in a loop (up to ARC_ACK_EVENT_TIMEOUT_MS):
- Skip non-0x20 packets (stale register responses).
- Skip 0x20 events where **bit0 = 0** (RECON/other, not our TX-complete).
- On first 0x20 event with **bit0 = 1** (TX-complete): check bit1 for ACK.
  - bit1 = 1 → ARC_OK
  - bit1 = 0 → ARC_NOT_ACKED

No exact b4 value comparisons.  Bit-mask only.  Correct for any b4 combination.

Timing: TX-complete event arrives within ~5–15 ms of WritePipe returning.

---

# UPDATE 12 — Receive is push-model: EP0x86 direct read, no EP0x02 poll

## Background

UPDATE 5 documented a 10-byte all-zeros exchange on EP0x02/EP0x86 during startup
and noted it was "likely" the receive-poll mechanism used during operation.  This
was adopted as the `arc_receive_locked` implementation.

UPDATE 3 explicitly flagged the question as unresolved:
> *Confirm whether host must issue a poll request per receive, or simply keep a
> blocking read posted on EP0x86.*

## Root-cause investigation (listen + transmit ACK conflict)

With `arc_listen_start` active, `arc_transmit` was returning `ARC_NOT_ACKED` even
though the packet reached the destination.  Diagnostic logging (ep81_diag.txt) showed:

- Exactly 1 EP0x81 event per TX, arriving at +0 ms (already queued).
- With listener active: b4=0x01 (no-ACK) most of the time.
- Without listener: b4=0x03 (ACK) consistently (20/20 in test_txloop).

The mechanism: `arc_receive_locked` writes 10 all-zeros to EP0x02.  The device
firmware interprets the frame as a long-frame TX to node 0 (byte1=0x00 = long-frame
marker, byte2=0x00 → L=512, which is invalid/rejected by ARCNET).  Regardless of
whether the frame hits the bus, the firmware generates a TX-complete event on EP0x81
with b4=0x01 (no-ACK — node 0 does not exist).  This stale event sits in the EP0x81
queue.  When `arc_transmit` subsequently reads EP0x81, it consumes the stale b4=0x01
first (FIFO), returns `ARC_NOT_ACKED`, and never reads the real b4=0x03 event from
its own TX.

Side effect (network pollution concern): if the all-zeros write does generate an
actual ARCNET frame, ~40 spurious TX/s per listening device (25 ms poll cycle) would
accumulate with multiple devices.  Not confirmed via USBPcap because the root-cause
fix made the verification moot.

## Fix: push-model receive (EP0x86 direct read)

`arc_receive_locked` was modified to **remove the EP0x02 all-zeros write entirely**
and post a direct `WinUsb_ReadPipe` on EP0x86 only:

```
Old: EP0x02 OUT (10B all-zeros) → EP0x86 IN (packet or all-zeros if none)
New: EP0x86 IN  (blocks until packet arrives or PIPE_TRANSFER_TIMEOUT)
```

## Verification

- **GUI Phase 4 live receive panel**: packets received correctly in both directions
  with listener active.
- **arc_transmit ACK**: b4=0x03 (ARC_OK) with listener active after fix; stale
  b4=0x01 events no longer appear.
- **test_multi FULL PASS**: bidirectional synchronous `arc_receive` continues to
  work — EP0x86 delivers packets without the EP0x02 trigger.
- No spurious EP0x81 events generated during receive polling.

## Conclusion

The device firmware uses a **push model** for receive: inbound ARCNET frames are
delivered on EP0x86 IN as they arrive; no EP0x02 poll is required.  The original
CC driver's all-zeros EP0x02 write was a startup handshake artifact, not a mandatory
receive-poll protocol.  Confirmed closed: UPDATE 3 open question resolved.
