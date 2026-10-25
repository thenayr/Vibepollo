# Audio bitstream passthrough

Vibepollo can send a Dolby MAT (Dolby Atmos) bitstream to a Moonlight client instead
of Opus, so the client can pass it over HDMI to an Atmos receiver, TV or soundbar
unchanged. Opus can't carry Atmos: it mixes to at most 7.1 PCM.

The bitstream comes from Windows itself. With **Dolby Atmos for home theater** active
on an endpoint, Windows renders all audio to it as an IEC 61937 Dolby MAT stream
(8 channels, 192 kHz, 16-bit container). Vibepollo needs the Vibepollo Atmos probe
driver for that endpoint: a virtual HD Audio device that copies every byte rendered
to it into a shared-memory tap. The host reads the tap, removes the zero stuffing
and sends what remains.

The same document lives in the host and the client forks. Change them together.
Host-side constants are in `src/audio_bitstream_protocol.h`; the frame format is in
`src/audio_bitstream.h`.

## Negotiation

| Direction | Attribute | Meaning |
|---|---|---|
| Host, RTSP DESCRIBE | `a=x-ss-audio.bitstreamFormats:<mask>` | Formats the host can capture now. Sent only when the probe's tap is open. |
| Client, RTSP ANNOUNCE | `x-ss-audio.bitstreamFormats:<mask>` | Formats the client can play. |

| Format bit | Name | Meaning |
|---|---|---|
| `0x1` | `FORMAT_IEC61937` | Packed IEC 61937 frames (frame format version 1) in 1200-byte chunks |

The session uses passthrough when both masks contain `FORMAT_IEC61937`, unless the
client asked for host audio (passthrough routes system audio to the silent probe
endpoint). Otherwise it's an ordinary Opus session. Clients must still send the
usual surround attributes so the Opus fallback works.

When passthrough is on, the host makes the probe the default output for the session
and restores the previous default afterwards, as it does for its virtual sink. It
doesn't change the probe's format. It reserves 3000 kbit/s of the video bitrate for
audio instead of the Opus estimate.

## RTP

Passthrough uses the normal audio socket, encryption (`SS_ENC_AUDIO`, AES-CBC) and
4 data + 2 parity Reed-Solomon FEC.

- Data packets use RTP payload type **100** (`RTP_PAYLOAD_TYPE`); Opus stays 97.
  FEC packets keep type 127 with `100` in their FEC header's payload type.
- Every payload is exactly **1200 bytes** before encryption (`CHUNK_BYTES`), so FEC
  shards are equal-sized. With RTP and AES-CBC padding a datagram is 1228 bytes.
- RTP timestamps advance by the negotiated packet duration per packet, as for Opus.
  They carry no timing for passthrough.
- The host sends nothing while the probe has no stream, or while it receives PCM
  (Atmos for home theater off).

## Chunks

Each packed frame is cut into chunks. All fields are little-endian.

| Offset | Size | Field |
|---|---|---|
| 0 | 1 | Version, `1` |
| 1 | 1 | Frame flags (below) |
| 2 | 2 | Frame id, increments per frame and wraps |
| 4 | 2 | Chunk index, from 0 |
| 6 | 2 | Chunk count: `ceil(frame length / 1188)`, or 1 for an empty frame |
| 8 | 4 | Packed frame length in bytes, at most 524288 |
| 12 | 1188 | Frame bytes; the last chunk is zero-padded |

A receiver drops a frame that is missing any chunk, rejects chunks whose header is
inconsistent, and marks the next complete frame as a discontinuity.

## Packed frames

A frame is one IEC 61937 burst period of the raw stream: 61440 bytes (20 ms) for
MAT. The host cuts the stream at burst preambles (`72 F8 1F 4E` on 16-bit sample
boundaries), then stores each frame as zero runs and data runs:

| Offset | Size | Field |
|---|---|---|
| 0 | 1 | Version, `1` |
| 1 | 1 | Frame flags |
| 2 | 2 | Segment count |
| 4 | 4 | Raw length in bytes, at most 262144 |
| 8 | ... | Segments: `u16 zero bytes to skip`, `u16 data length`, data |

Bytes after the last segment, up to the raw length, are zero. A receiver rebuilds
the exact raw bytes and must reject a frame whose segments run past the raw length
or the packet, or that has bytes left over.

| Flag | Name | Meaning |
|---|---|---|
| `0x01` | `burst_aligned` | The frame starts at an IEC 61937 burst preamble |
| `0x02` | `discontinuity` | Not contiguous with the previous frame: a new stream, or data was lost |

The rebuilt bytes are the IEC 61937 stream exactly as Windows rendered it. A client
plays them as 8-channel, 192 kHz, 16-bit PCM to an HDMI output in passthrough
(non-audio) mode. After a discontinuity it should wait for a `burst_aligned` frame
before resuming output.

## Measured

A 35 s Dolby MAT 2.1 capture packed 14.8:1. Cut into chunks it was about 210
packets/s, 2.0 Mbit/s of payload and 3.0 Mbit/s with FEC, at most 5 chunks per
20 ms frame.
