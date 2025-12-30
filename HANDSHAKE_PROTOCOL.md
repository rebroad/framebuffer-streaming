# Framebuffer Streaming Handshake Protocol

This document describes the byte-by-byte handshake protocol between the X11 streamer (server) and the Android TV receiver (client).

## Overview

The handshake establishes a secure connection and exchanges display capabilities. The flow differs based on whether encryption is required (determined by the network interface).

## Message Format

All messages use a common header format:

```
Message Header (9 bytes):
+-------+----+----+----+----+----+----+----+----+
| Type  |   Length (uint32, |     Sequence      |
|(uint8)|   little-endian)  |     (uint32)      |
+-------+----+----+----+----+----+----+----+----+
  0x00   0x01 0x02 0x03 0x04 0x05 0x06 0x07 0x08
```

- **Type**: Message type (see `message_type_t` enum)
- **Length**: Payload length in bytes (not including header)
- **Sequence**: Sequence number (currently always 0)

All multi-byte integers are in **big-endian** (network byte order).

## Handshake Flow

### Phase 1: TCP Connection

```
Streamer                    Receiver
   |                            |
   |-------- TCP Connect ------>|
   |                            |
```

The streamer establishes a TCP connection to the receiver on port 4321 (default).

### Phase 2: Capabilities Exchange

#### Step 1: Receiver sends CAPABILITIES

**Receiver → Streamer: MSG_CAPABILITIES (0x14)**

```
Header (9 bytes):
+-------+-------+-------+-------+-------+-------+-------+-------+-------+
| 0x14  | 0x04  | 0x00  | 0x00  | 0x00  | 0x00  | 0x00  | 0x00  | 0x00  |
+-------+-------+-------+-------+-------+-------+-------+-------+-------+
  Type    Length=4 bytes (little-endian)            Sequence=0

Payload (4 bytes):
+--------+--------+--------+--------+
|requires| 0x00   | 0x00   | 0x00   |
|encrypt | reserved[1]     | reserved[2]|
+--------+--------+--------+--------+
  uint8    uint8    uint8    uint8
```

- `requires_encryption`: 0x00 = false (USB tethering), 0x01 = true (WiFi hotspot/public WiFi)
- `reserved[3]`: Reserved for future capabilities

**Example (USB tethering, no encryption):**
```
14 00 00 00 04 00 00 00 00 00 00 00 00
```

**Example (WiFi hotspot, encryption required):**
```
14 00 00 00 04 00 00 00 00 01 00 00 00
```

### Phase 3: Encryption (if required)

If `requires_encryption == 0x01`, the Noise Protocol Framework handshake is performed.

#### Noise Protocol Pattern: `Noise_NK_25519_ChaChaPoly_SHA256`

- **Pattern**: NK (No static key for initiator, static key for responder)
- **DH**: Curve25519
- **Cipher**: ChaChaPoly (ChaCha20-Poly1305)
- **Hash**: SHA256

**Noise_NK Pattern Handshake:**

```
Streamer (Initiator)       Receiver (Responder)
     |                          |
     |-------- e, es ---------->|  Message 1: Initiator's ephemeral key + encrypted static key
     |                          |
     |<------- e, ee -----------|  Message 2: Responder's ephemeral key + encrypted ephemeral keys
     |                          |
```

**Message Format for Noise Handshake:**

Each Noise handshake message is prefixed with a 2-byte length (big-endian, network byte order):

```
+--------------+--------------+--------+...
| Length (uint16, big-endian) | Noise Message Data
+--------------+--------------+--------+...
```

**Noise Message 1 (Streamer → Receiver):**

```
+--------+--------+---+---+---+----+--+--+--+--+--+--+-------+--------+...
| Length | Length |  e (32 bytes)  |  es (48 bytes)  | MAC (16 bytes) |
+--------+--------+---+---+---+----+--+--+--+--+--+--+-------+--------+...
  uint16  (high)   Ephemeral pubkey  Encrypted static   Poly1305 tag
  (big-endian)
```

- `e`: Streamer's ephemeral public key (32 bytes, Curve25519)
- `es`: Encrypted receiver's static public key (32 bytes) + MAC (16 bytes)
- Total: ~98 bytes + 2-byte length prefix (big-endian)

**Noise Message 2 (Receiver → Streamer):**

```
+--------+--------+--------+--------+--------+--------+--------+--------+...
| Length | Length |  e (32 bytes)   |  ee (48 bytes)  | MAC (16 bytes)  |
+--------+--------+--------+--------+--------+--------+--------+--------+...
  uint16  (high)   Ephemeral pubkey   Encrypted keys    Poly1305 tag
  (big-endian)
```

- `e`: Receiver's ephemeral public key (32 bytes, Curve25519)
- `ee`: Encrypted ephemeral keys (32 bytes) + MAC (16 bytes)
- Total: ~98 bytes + 2-byte length prefix (big-endian)

After the Noise handshake completes, all subsequent messages are encrypted using the established cipher states.

### Phase 4: PIN Verification (if encryption enabled)

#### Step 1: Streamer sends PIN_VERIFY

**Streamer → Receiver: MSG_PIN_VERIFY (0x12)**

If encryption is enabled, this message is **encrypted** using Noise Protocol.

```
Header (9 bytes) - encrypted:
+--------+--------+--------+--------+--------+-----+-----+-----+-----+
| 0x12   | Length | Length | Length | Length | Sequence (4 bytes)    |
+--------+--------+--------+--------+--------+-----+-----+-----+-----+
  Type    Length=2 bytes (little-endian)                    Sequence

Payload (2 bytes) - encrypted:
+--------+--------+
|  PIN   |  PIN   |
| (low)  | (high) |
+--------+--------+
  uint16 (little-endian)
```

- PIN is a 4-digit number (0000-9999) entered by the user
- Example: PIN 1234 = `0xD2 0x04` (little-endian)

**Encrypted Message Format:**

The encrypted message is wrapped with Noise Protocol encryption:

```
+--------+--------+--------+--------+--------+--------+...
| Length | Length |  Encrypted Header (9 bytes) + Payload (2 bytes) + MAC (16 bytes)
+--------+--------+--------+--------+--------+--------+...
  uint16  (high)   Noise-encrypted data
  (big-endian)
```

#### Step 2: Receiver sends PIN_VERIFIED

**Receiver → Streamer: MSG_PIN_VERIFIED (0x13)**

If encryption is enabled, this message is **encrypted** using Noise Protocol.

```
Header (9 bytes) - encrypted:
+--------+--------+--------+--------+--------+--------+--------+--------+--------+
| 0x13   | 0x00   | 0x00   | 0x00   | 0x00   | 0x00   | 0x00   | 0x00   | 0x00   |
+--------+--------+--------+--------+--------+--------+--------+--------+--------+
  Type    Length=0 (no payload)                              Sequence=0
```

**Encrypted Message Format:**

```
+--------+--------+--------+--------+--------+--------+...
| Length | Length |  Encrypted Header (9 bytes) + MAC (16 bytes)
+--------+--------+--------+--------+--------+--------+...
  uint16  (high)   Noise-encrypted data
  (big-endian)
```

### Phase 5: Display Capabilities Exchange

#### Step 1: Receiver sends HELLO

**Receiver → Streamer: MSG_HELLO (0x01)**

If encryption is enabled, this message is **encrypted** using Noise Protocol.

```
Header (9 bytes):
+--------+--------+--------+--------+--------+--------+--------+--------+--------+
| 0x01   |              Length (variable)              |  Sequence (0x00 0x00 0x00 0x00)
+--------+--------+--------+--------+--------+--------+--------+--------+--------+
  Type    Payload length (little-endian)                    Sequence=0

Payload Structure:
+--------+--------+--------+--------+--------+--------+--------+--------+--------+
|Protocol|Protocol| num    | num    | name   | name   | Display Name (variable) |
|version |version | modes  | modes  | len    | len    | (null-terminated)      |
|(low)   |(high)  |(low)   |(high)  |(low)   |(high)  |                         |
+--------+--------+--------+--------+--------+--------+--------+--------+--------+
  uint16            uint16            uint16

+--------+--------+--------+--------+--------+--------+--------+--------+--------+
|  Mode 1: width (4 bytes) | height (4 bytes) | refresh_rate (4 bytes)         |
+--------+--------+--------+--------+--------+--------+--------+--------+--------+
  uint32 (little-endian)    uint32              uint32 (Hz * 100)

+--------+--------+--------+--------+--------+--------+--------+--------+--------+
|  Mode 2: width (4 bytes) | height (4 bytes) | refresh_rate (4 bytes)         |
+--------+--------+--------+--------+--------+--------+--------+--------+--------+
  ... (repeated for each mode)
```

**Field Descriptions:**

- `protocol_version`: Currently 1 (uint16, little-endian)
- `num_modes`: Number of display modes (uint16, little-endian)
- `display_name_len`: Length of display name including null terminator (uint16, little-endian)
- `display_name`: UTF-8 string, null-terminated
- `modes`: Array of `display_mode_t` structures:
  - `width`: Display width in pixels (uint32, little-endian)
  - `height`: Display height in pixels (uint32, little-endian)
  - `refresh_rate`: Refresh rate in Hz * 100 (uint32, little-endian)
    - Example: 60.00 Hz = 6000, 120.00 Hz = 12000

**Example (unencrypted, single mode):**

Display: "Phone Display", 1920x1080@60Hz

```
Header:
01 00 00 00 1D 00 00 00 00

Payload (29 bytes = 0x1D):
00 01          // protocol_version = 1 (big-endian)
00 01          // num_modes = 1 (big-endian)
00 0F          // display_name_len = 15 ("Phone Display" + null) (big-endian)
50 68 6F 6E 65 20 44 69 73 70 6C 61 79 00  // "Phone Display\0"
00 00 07 80    // width = 1920 (0x0780) (big-endian)
00 00 04 38    // height = 1080 (0x0438) (big-endian)
00 00 17 70    // refresh_rate = 6000 (0x1770 = 60.00 Hz * 100) (big-endian)
```

**Complete message (hex):**
```
01 00 00 00 1D 00 00 00 00 00 01 00 01 00 0F 50 68 6F 6E 65 20 44 69 73 70 6C 61 79 00 00 00 07 80 00 00 04 38 00 00 17 70
```

**Example (encrypted):**

If encryption is enabled, the entire HELLO message (header + payload) is encrypted using Noise Protocol:

```
+--------+--------+--------+--------+--------+--------+...
| Length | Length |  Encrypted Header (9 bytes) + Payload + MAC (16 bytes)
+--------+--------+--------+--------+--------+--------+...
  uint16  (high)   Noise-encrypted data
  (big-endian)
```

## Complete Handshake Examples

### Example 1: USB Tethering (No Encryption)

```
Streamer                    Receiver
   |                            |
   |-------- TCP Connect ------>|
   |                            |
   |<------- CAPABILITIES -----|  requires_encryption=0
   |                            |
   |<------- HELLO ------------|  Unencrypted
   |                            |
   |-------- Ready ------------>|  Start streaming frames
```

**Byte Sequence:**

1. **CAPABILITIES** (13 bytes):
   ```
   14 00 00 00 04 00 00 00 00 00 00 00 00
   ```

2. **HELLO** (38 bytes for "Phone Display" 1920x1080@60Hz):
   ```
   01 00 00 00 1D 00 00 00 00 00 01 00 01 00 0F 50 68 6F 6E 65 20 44 69 73 70 6C 61 79 00 00 00 07 80 00 00 04 38 00 00 17 70
   ```

### Example 2: WiFi Hotspot (With Encryption)

```
Streamer                    Receiver
   |                            |
   |-------- TCP Connect ------>|
   |                            |
   |<------- CAPABILITIES -----|  requires_encryption=1
   |                            |
   |-------- Noise Msg 1 ------>|  e, es
   |                            |
   |<------- Noise Msg 2 -------|  e, ee
   |                            |
   |-------- PIN_VERIFY ------->|  Encrypted (PIN: 1234)
   |                            |
   |<------- PIN_VERIFIED ------|  Encrypted
   |                            |
   |<------- HELLO ------------|  Encrypted
   |                            |
   |-------- Ready ------------>|  Start streaming frames
   ```

**Byte Sequence:**

1. **CAPABILITIES** (13 bytes):
   ```
   14 00 00 00 04 00 00 00 00 01 00 00 00
   ```

2. **Noise Message 1** (~100 bytes):
   ```
   [Length: 2 bytes] [e: 32 bytes] [es: 48 bytes]
   ```

3. **Noise Message 2** (~100 bytes):
   ```
   [Length: 2 bytes] [e: 32 bytes] [ee: 48 bytes]
   ```

4. **PIN_VERIFY** (encrypted, ~35 bytes):
   ```
   [Length: 2 bytes, big-endian] [Encrypted: Header (9) + PIN (2) + MAC (16)]
   ```

5. **PIN_VERIFIED** (encrypted, ~27 bytes):
   ```
   [Length: 2 bytes, big-endian] [Encrypted: Header (9) + MAC (16)]
   ```

6. **HELLO** (encrypted, ~55 bytes):
   ```
   [Length: 2 bytes, big-endian] [Encrypted: Header (9) + Payload (29) + MAC (16)]
   ```

## Message Type Constants

```c
MSG_HELLO = 0x01
MSG_FRAME = 0x02
MSG_AUDIO = 0x03
MSG_CONFIG = 0x05
MSG_PING = 0x06
MSG_PONG = 0x07
MSG_PAUSE = 0x08
MSG_RESUME = 0x09
MSG_DISCOVERY_REQUEST = 0x10
MSG_DISCOVERY_RESPONSE = 0x11
MSG_PIN_VERIFY = 0x12
MSG_PIN_VERIFIED = 0x13
MSG_CAPABILITIES = 0x14
MSG_ERROR = 0xFF
```

## Notes

1. **Endianness**: All multi-byte integers in application protocol messages are in **big-endian** (network byte order) for portability across different CPU architectures.

2. **Encryption**: When encryption is enabled (Noise Protocol), all messages after the handshake are encrypted. The encrypted data includes:
   - Original message header (9 bytes)
   - Original message payload (variable)
   - Poly1305 authentication tag (16 bytes)
   - All encrypted messages are prefixed with a 2-byte length (big-endian, network byte order)

3. **Noise Protocol**: Uses `Noise_NK_25519_ChaChaPoly_SHA256` pattern:
   - Streamer is the initiator (ephemeral key)
   - Receiver is the responder (static key)
   - Provides authenticated encryption

4. **PIN Verification**: Only occurs when encryption is enabled. The PIN is a 4-digit number (0000-9999) displayed on the receiver's screen and entered by the user on the streamer.

5. **Display Modes**: The receiver can advertise multiple display modes (resolutions and refresh rates). The streamer creates a virtual X11 output with all these modes available via `xrandr`.

