# AIC Pico AIME Reader Protocol and Timing

This document describes the Sega AIME serial protocol implemented by AIC Pico,
with emphasis on card polling, FeliCa operations, the Amusement IC random
challenge flow, and timing behavior.

The implementation described here is:

- `firmware/src/lib/aime.c`: AIME framing, commands, and responses
- `firmware/src/lib/nfc.c`: card detection, card priority, and shared NFC logic
- `firmware/src/lib/pn532.c`: PN532 transport and retries
- `firmware/src/lib/pn5180.c`: PN5180 transport and timing
- `../tools/reader_comm.py`: example host implementation

This is an implementation reference, not an official Sega protocol
specification. Some responses intentionally favor game compatibility over
strictly reporting hardware errors.

## Quick Reference

| Item | Value |
| --- | --- |
| USB interface | `AIC Pico AIME Port` |
| Serial format | 8 data bits, no parity, 1 stop bit |
| Low-speed AIME mode | 38400 baud, firmware byte `92`, identity `837-15286EXP` |
| High-speed AIME mode | 115200 baud, firmware byte `94`, identity `837-15396` |
| Frame sync byte | `E0` |
| Escape byte | `D0` |
| Checksum | Sum of the unescaped body bytes, modulo 256 |
| Normal host response window | Up to 1 second |
| Suggested card-detect interval | 150 to 250 ms |
| Suggested delay after polling starts | 50 ms |

The baud rate is line-coding metadata on the USB CDC interface. It selects the
emulated reader identity and AIME sub-mode; it does not change USB transfer
speed.

## Reader Modes

In automatic mode, AIC Pico identifies the reader protocol from the first
bytes sent by the host:

- A frame beginning with `E0` and a small frame length selects AIME.
- At 115200 baud it selects AIME1/high-speed mode.
- Other AIME baud rates normally select AIME0/low-speed mode.
- Bandai Namco reader traffic is detected separately.

The active AIME identity is selected from the current baud rate:

| Baud | Mode | Firmware response | Hardware response |
| --- | --- | --- | --- |
| 38400 | AIME0 | `92` | `837-15286EXP` |
| 115200 | AIME1 | `94` | `837-15396` |

The mode can also be forced from the CLI:

```text
mode auto
mode aime0
mode aime1
```

## Wire Framing

Every frame starts with `E0`. The remaining body and checksum are escaped
before being placed on the wire.

### Request Body

| Offset | Size | Field |
| --- | --- | --- |
| 0 | 1 | Length, equal to `5 + payload length` |
| 1 | 1 | Address |
| 2 | 1 | Sequence number |
| 3 | 1 | Command |
| 4 | 1 | Payload length |
| 5 | N | Payload |
| End | 1 | Checksum |

### Response Body

| Offset | Size | Field |
| --- | --- | --- |
| 0 | 1 | Length, equal to `6 + payload length` |
| 1 | 1 | Address copied from request |
| 2 | 1 | Sequence number copied from request |
| 3 | 1 | Command copied from request |
| 4 | 1 | Status |
| 5 | 1 | Payload length |
| 6 | N | Payload |
| End | 1 | Checksum |

The length byte does not include the sync byte or checksum. A complete decoded
frame therefore contains `length + 1` bytes after the sync byte.

### Checksum

The checksum is the low byte of the sum of every unescaped body byte, including
the length byte:

```text
checksum = sum(body) & 0xff
```

### Escaping

Only bytes after the sync byte are escaped:

| Original byte | Bytes on wire |
| --- | --- |
| `E0` | `D0 DF` |
| `D0` | `D0 CF` |

For either reserved byte, write `D0`, then write the original byte minus one.
The receiver reverses this by adding one to the byte after `D0`.

### Example: Card Detect With No Card

Host request:

```text
E0 05 00 19 42 00 60
```

Decoded request:

```text
length=05 address=00 sequence=19 command=42 payload_length=00 checksum=60
```

Reader response:

```text
E0 07 00 19 42 00 01 00 63
```

Decoded response:

```text
length=07 address=00 sequence=19 command=42
status=00 payload_length=01 payload=00 checksum=63
```

The host should increment the sequence number for each command. The reader
echoes the request address, sequence number, and command.

## Status Values

| Status | Name | Meaning |
| --- | --- | --- |
| `00` | OK | Command accepted |
| `01` | CARD_ERROR | Required card was not available |
| `03` | INVALID_COMMAND | Command or FeliCa sub-command is unsupported |

Some compatibility commands return `OK` even when no meaningful operation is
performed. Do not treat `OK` alone as proof that a physical card write or
authentication succeeded.

## Commands

| Command | Name | Request payload | Response |
| --- | --- | --- | --- |
| `30` | GET_FW_VERSION | None | Mode-specific firmware bytes |
| `32` | GET_HW_VERSION | None | Mode-specific hardware string |
| `40` | START_POLLING | Usually `01` or `03`; value is ignored | Empty `OK` |
| `41` | STOP_POLLING | None | Empty `OK` |
| `42` | CARD_DETECT | None | Card information |
| `43` | CARD_SELECT | None | Empty `OK` |
| `44` | CARD_HALT | None | Empty `OK` |
| `50` | MIFARE_KEY_SET_A | Six-byte key | Empty `OK` |
| `51` | MIFARE_AUTHORIZE_A | UID and block | Empty compatibility response |
| `52` | MIFARE_READ | UID and block | 16 bytes |
| `54` | MIFARE_KEY_SET_B | Six-byte key | Empty `OK` |
| `55` | MIFARE_AUTHORIZE_B | UID and block | Empty compatibility response |
| `61` | SEND_HEX_DATA | Game-specific | Empty `OK` |
| `62` | TO_NORMAL_MODE | None | `INVALID_COMMAND` when reader is healthy |
| `70` | FELICA_PUSH | Game-specific | `INVALID_COMMAND` |
| `71` | FELICA_OP | Encapsulated FeliCa command | Encapsulated FeliCa response |
| `81` | EXT_BOARD_LED_RGB | Red, green, blue | Empty `OK` |
| `F0` | EXT_BOARD_INFO | None | LED board information |
| `F5` | EXT_TO_NORMAL_MODE | None | Empty `OK` |

Unknown commands currently receive an empty `OK` response for compatibility.

## Typical Game Initialization

A low-speed game capture commonly performs this sequence:

1. Set the reader port to 38400 baud.
2. Send LED color command `81`.
3. Send `TO_NORMAL_MODE` (`62`) and expect status `03`.
4. Read firmware with `30`.
5. Read hardware identity with `32`.
6. Initialize the external board with `F5` and `F0`.
7. Set MIFARE Key B to ASCII `WCCFv2`.
8. Set MIFARE Key A to `60 90 D0 06 32 F5`.
9. Enter the polling loop.

The common polling loop is:

```text
START_POLLING -> CARD_DETECT -> STOP_POLLING
```

The captured game often repeats the entire three-command loop rather than
leaving polling enabled continuously.

## Card Detect Response

### No Card

```text
00
```

| Field | Value |
| --- | --- |
| Count | `00` |

### MIFARE

```text
01 10 <uid_length> <uid>
```

| Field | Meaning |
| --- | --- |
| Count | `01` |
| Type | `10` |
| UID length | Usually 4 or 7 |
| UID | Raw ISO/IEC 14443-A UID |

### FeliCa

```text
01 20 10 <IDm:8> <PMm:8>
```

| Field | Meaning |
| --- | --- |
| Count | `01` |
| Type | `20` |
| ID length | `10`, meaning 16 bytes |
| IDm | Eight bytes |
| PMm | Eight bytes |

The system code is not included in `CARD_DETECT`; it is available through
FeliCa operations and internal NFC metadata.

## Card Detection Priority

The firmware deliberately handles stacked or overlapping cards:

1. Try MIFARE.
2. If the MIFARE card is a BanaPassport, return it immediately.
3. If it is a generic MIFARE card, try FeliCa up to three additional times.
4. Prefer the later FeliCa result over the generic MIFARE result.
5. If no MIFARE was detected, try FeliCa, then ISO/IEC 15693.

The extra FeliCa attempts have an 8 ms pause after each failed attempt. This
can add roughly 24 ms to detection when a generic MIFARE card is found first.

## Virtual AIC Behavior

Virtual AIC is enabled by default. It presents non-AIC cards to an AIME game as
FeliCa-compatible virtual cards.

| Physical card | Virtual IDm |
| --- | --- |
| Four-byte MIFARE UID | `01 01` + UID + first two UID bytes |
| Seven-byte MIFARE UID | `01 01` + first six UID bytes |
| Non-AIC FeliCa, including Suica phones | Original IDm |
| ISO/IEC 15693 | UID with first byte replaced by `01` |

Real Amusement IC FeliCa cards with system code `88B4` remain real FeliCa
cards. Suica/transit system code `0003` and FeliCa Lite-S system code `FE00`
are virtualized when Virtual AIC is enabled.

## FeliCa Operation Command

All FeliCa traffic is carried inside AIME command `71`.

### Outer Request Payload

```text
<outer_IDm:8> <encapsulated_length:1> <FeliCa_command:1> <FeliCa_data:N>
```

The encapsulated length includes the FeliCa command byte and its length byte.
For read, write, and Active2 traffic, the IDm also appears inside the FeliCa
data.

### Supported FeliCa Commands

| Code | Name | Response code |
| --- | --- | --- |
| `00` | Polling | `01` |
| `06` | Read Without Encryption | `07` |
| `08` | Write Without Encryption | `09` |
| `0C` | Request System Code | `0D` |
| `A4` | Active2 | `A5` |

### Read Without Encryption

Inner request data:

```text
<IDm:8>
<service_count:1>
<service_code_little_endian:2>
<block_count:1>
<block_list:2 * block_count>
```

The two-byte block list entries are sent high byte first. For example:

```text
service 000B -> 0B 00
block 8082   -> 80 82
```

Read response payload:

```text
<encapsulated_length>
07
<IDm:8>
<status1>
<status2>
<block_count>
<block_data:16 * block_count>
```

The implementation caps one request at eight blocks.

### Write Without Encryption

Inner request data:

```text
<IDm:8>
<service_count:1>
<service_code_little_endian:2>
<block_count:1>
<block_list:2>
<block_data:16>
```

Write response payload:

```text
0C 09 <IDm:8> 00 00
```

The current compatibility implementation returns FeliCa status `00 00` even
when the underlying physical write fails.

## Amusement IC Random Challenge Flow

A successful Amusement IC login uses a fresh 16-byte random challenge for each
read. The reader exchange observed in the game capture is:

1. Detect a FeliCa card and obtain its IDm and PMm.
2. Stop the normal polling loop.
3. Send FeliCa Polling through command `71`.
4. Read service `000B`, block `8082`.
5. Write the 16-byte random challenge to service `0009`, block `8080`.
6. Read service `000B`, blocks `8082`, `8086`, `8090`, and `8091` together.
7. Optionally read another card-specific block such as `8000`.
8. Stop polling or halt the session.

The important blocks used by the AccountS request are:

| Service | Block | AccountS field | Size |
| --- | --- | --- | --- |
| `000B` | `8082` | ID | 16 bytes |
| `000B` | `8086` | CKV | 16 bytes |
| `000B` | `8090` | WCNT | 16 bytes |
| `000B` | `8091` | Challenge-dependent authentication data / MACA source | 16 bytes |

Challenge write example:

```text
service = 0009
block   = 8080
RC      = 49 44 0C F8 1B EF EC F6 84 C3 7E 57 5D 8C 6E E8
```

The next multi-block read must belong to the same card presentation and
challenge session.

### Virtual and Fallback Data

For Virtual AIC:

- Writing service `0009`, block `8080` refreshes the virtual session from the
  challenge and the Pico clock.
- Reading block `8091` returns a changing 16-byte virtual session value.
- Synthetic blocks `8082`, `8086`, and `8090` provide the minimum AIC-shaped
  data expected by the game.

For a real card, the firmware first attempts the physical operation. If block
`8091` cannot be read, it returns eight generated session bytes followed by
eight zero bytes. This fallback is useful for compatibility, but it is not a
cryptographic MAC produced by the physical card.

### Building the AiMeDB AccountS Payload

AccountS is a network request, not part of the serial reader protocol. It is
normally assembled from the challenge and card blocks:

```text
RC[16]
ID[16]
CKV[16]
WCNT[16]
MACA[8]
company_code[1]
reader_firmware[1]
DFC[2, little-endian]
```

Total payload length: 76 bytes.

## Timing

### Recommended Host Timing

These values are conservative defaults used by `tools/reader_comm.py` and work
well for standalone testing:

| Setting | Recommended value |
| --- | --- |
| Serial read/write timeout | 50 ms |
| Idle gap used to finish collecting a response | 100 ms |
| Maximum response window | 1 second |
| Delay after `START_POLLING` | 50 ms |
| Delay between unsuccessful `CARD_DETECT` attempts | 200 ms |
| Total wait for a user to present a card | 3 seconds or longer |

Always wait for the complete response to one command before sending the next
command. The response is the primary synchronization point.

For marginal PN532 modules or phone-based FeliCa, an optional 5 to 10 ms guard
delay between FeliCa operations can improve reliability. Do not add a large
fixed delay unless a capture shows the game requires it.

### Firmware Timing and Limits

| Behavior | Timing |
| --- | --- |
| Main firmware service loop | Approximately 1 ms |
| PN532 ready polling interval | 1 ms |
| PN532 ready timeout | Approximately 150 ms per readiness wait |
| PN532 FeliCa operation retries | Three attempts |
| Delay after failed PN532 FeliCa attempt | 5 ms |
| PN5180 busy timeout | 100 ms |
| PN5180 busy polling interval | 10 us |
| PN5180 FeliCa poll/read/write settle | 1 ms |
| Later-FeliCa priority attempts | Three attempts with 8 ms pauses |
| NFC initialization retries | Three attempts with 200 ms pauses |
| Last-card metadata lifetime | 1 second |
| AIME protocol active lifetime after valid frame | 1200 seconds |
| AIME lifetime after unexpected DTR-off | Three seconds |

A PN532 command can encounter three readiness waits: one for its ACK and two
while locating and reading the response. The worst path can therefore approach
450 ms before retries and transfer overhead. A 1-second response window is the
safer default for card and FeliCa operations.

### DTR Timing

The firmware observes DTR changes to decide when an AIME session should expire.
It tolerates expected DTR-off events around:

| Trigger | Expected DTR-off time | Accepted window |
| --- | --- | --- |
| `FELICA_PUSH` (`70`) | About 50 ms later | Plus or minus 70 ms |
| LED RGB (`81`) | About 400 ms later | Plus or minus 70 ms |

An unexpected DTR-off while AIME is active shortens the active session lifetime
to three seconds. Opening a second serial monitor on the game port can disturb
the session or prevent the game from opening the port at all.

### Captured Game Behavior

The Windows game capture configured:

- 38400 baud
- DTR control
- Zero Windows serial timeout fields

The game did not rely on a single blocking read. It repeatedly checked the COM
queue and read complete responses when bytes became available. A standalone
tool does not need to copy that exact Windows behavior; using a bounded
response window and parsing complete frames is simpler and reliable.

## PN532 and PN5180 Differences

### PN532

- Connected over I2C in the common AIC Pico build.
- Waits for module readiness in 1 ms steps.
- Retries failed FeliCa commands up to three times.
- Can read MIFARE and FeliCa.
- Phone and stacked-card reliability is sensitive to antenna position, module
  quality, I2C stability, and command timing.

### PN5180

- Connected over SPI.
- Polls FeliCa twice and requires the second result to return the same IDm.
- Supports MIFARE, FeliCa, and ISO/IEC 15693.
- Mainstream modules with a 27 MHz crystal instead of 27.12 MHz can have poor
  NFC reliability.

## Testing With the Python Host

Run these commands from the workspace root, one directory above `aic_pico`.

Install PySerial:

```powershell
py -m pip install pyserial
```

List ports:

```powershell
python tools/reader_comm.py list
```

Read firmware and hardware identity:

```powershell
python tools/reader_comm.py aime --port COM8 --baud 38400 fw
python tools/reader_comm.py aime --port COM8 --baud 38400 hw
```

Detect a card:

```powershell
python tools/reader_comm.py aime --port COM8 --baud 38400 read-card --raw-io
```

Perform the random-challenge AIC read:

```powershell
python tools/reader_comm.py aime --port COM8 --baud 38400 read-aic --raw-io
```

Use a fixed challenge for capture comparison:

```powershell
python tools/reader_comm.py aime --port COM8 --baud 38400 read-aic `
  --challenge "49 44 0C F8 1B EF EC F6 84 C3 7E 57 5D 8C 6E E8" `
  --raw-io
```

## Troubleshooting

### No Response

- Confirm the game/tool is using `AIC Pico AIME Port`, not the CLI port.
- Confirm no serial monitor or second process already owns the port.
- Try 38400 first, then 115200.
- Verify sync, escaping, length, and checksum before debugging commands.
- Allow up to one second for card and FeliCa operations.

### Intermittent FeliCa Failure

- Wait for each complete response before sending the next operation.
- Use a 1-second response window.
- Add a small 5 to 10 ms guard between FeliCa operations only if necessary.
- Keep the phone or card stationary until the full challenge/read sequence
  completes.
- Check PN532 power, I2C wiring, pull-ups, and antenna clearance.
- Avoid placing metal, a USB connector, or another large antenna directly
  against the NFC antenna.

### Card Behind Another Card

- BanaPassport is intentionally returned immediately when positively
  identified.
- A generic MIFARE result gets up to three later FeliCa attempts.
- Presenting multiple FeliCa cards at once remains ambiguous and should be
  avoided.

### Suica Phone Detected but Login Fails

- Ensure Virtual AIC is enabled if the game expects Amusement IC behavior.
- Keep the phone awake and stationary through all FeliCa operations.
- A Suica phone is not a real Amusement IC card; successful detection does not
  guarantee that every game or server authentication path accepts it.

### Reader Error When Monitoring

The game requires exclusive access to its reader COM port. Passive monitoring
software must not open the physical reader port as a second active client.
Capture at the driver/filter level, or use a forwarding setup that preserves a
single owner of the physical port.
