#!/usr/bin/env python3
"""IMA ADPCM codec, for evaluating 4:1 compression of the soundtrack.

IMA ADPCM stores 4 bits per sample instead of 16: a step index walks up and
down a table according to how large each difference was, so loud passages get
coarse steps and quiet ones fine steps. It is sequential-only -- you cannot
seek into it without decoding from a block boundary -- which is fine for a
looping music stream and is why blocks exist at all.

The decoder is about forty lines of integer arithmetic, roughly ten
operations per sample. At 22050 that is a fifth of a million operations a
second, against the twenty-odd million the FM synthesis was costing.

Used offline here to judge quality; the shipping decoder would live in the
clip player.

    python adpcm.py roundtrip <in.wav> [out.wav]
"""
import array, struct, sys

STEP = [
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37,
    41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173,
    190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658,
    724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066,
    2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484,
    7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500,
    20350, 22385, 24623, 27086, 29794, 32767,
]
INDEX = [-1, -1, -1, -1, 2, 4, 6, 8]

BLOCK = 505          # samples per block: 4 header bytes + 252 data bytes


def encode(samples, block=BLOCK):
    """Each block restates the predictor, so a loop restarts cleanly."""
    out = bytearray()
    i, n = 0, len(samples)
    while i < n:
        pred = samples[i]
        idx = 0
        if i + 1 < n:
            d = abs(samples[i + 1] - pred)
            while idx < 88 and STEP[idx] < d // 2:
                idx += 1
        out += struct.pack("<hBB", pred, idx, 0)
        nib = []
        for s in samples[i + 1:i + block]:
            step = STEP[idx]
            diff = s - pred
            code = 0
            if diff < 0:
                code = 8
                diff = -diff
            tmp = step
            if diff >= tmp:
                code |= 4
                diff -= tmp
            tmp >>= 1
            if diff >= tmp:
                code |= 2
                diff -= tmp
            tmp >>= 1
            if diff >= tmp:
                code |= 1
            # Mirror the decoder exactly or the predictor drifts apart.
            delta = step >> 3
            if code & 4:
                delta += step
            if code & 2:
                delta += step >> 1
            if code & 1:
                delta += step >> 2
            pred = pred - delta if code & 8 else pred + delta
            pred = max(-32768, min(32767, pred))
            idx = max(0, min(88, idx + INDEX[code & 7]))
            nib.append(code)
        for k in range(0, len(nib) - 1, 2):
            out.append(nib[k] | (nib[k + 1] << 4))
        if len(nib) % 2:
            out.append(nib[-1])
        i += block
    return bytes(out)


def decode(data, block=BLOCK):
    out = array.array("h")
    per_block = 4 + block // 2
    i = 0
    while i + 4 <= len(data):
        pred, idx, _ = struct.unpack("<hBB", data[i:i + 4])
        out.append(pred)
        for byte in data[i + 4:i + per_block]:
            for code in (byte & 0x0F, byte >> 4):
                step = STEP[idx]
                delta = step >> 3
                if code & 4:
                    delta += step
                if code & 2:
                    delta += step >> 1
                if code & 1:
                    delta += step >> 2
                pred = pred - delta if code & 8 else pred + delta
                pred = max(-32768, min(32767, pred))
                idx = max(0, min(88, idx + INDEX[code & 7]))
                out.append(pred)
        i += per_block
    return out


def wav_write(path, samples, rate):
    raw = samples.tobytes()
    with open(path, "wb") as f:
        f.write(b"RIFF" + struct.pack("<I", 36 + len(raw)) + b"WAVEfmt ")
        f.write(struct.pack("<IHHIIHH", 16, 1, 1, rate, rate * 2, 2, 16))
        f.write(b"data" + struct.pack("<I", len(raw)) + raw)


def container(samples, rate, block=BLOCK):
    """The shipping format. A fixed-size block layout means the decoder can
    find any block by multiplication, and the per-block predictor is what lets
    a loop restart without a click."""
    body = encode(samples, block)
    head = b"JADP" + struct.pack("<HHII", 1, block, len(samples), rate)
    assert len(head) == 16, len(head)
    return head + body


def main():
    import wave, math
    if sys.argv[1] == "encode":
        # encode <in.raw> <out.adp> <rate>
        src, dst, rate = sys.argv[2], sys.argv[3], int(sys.argv[4])
        d = array.array("h")
        d.frombytes(open(src, "rb").read())
        blob = container(d, rate)
        open(dst, "wb").write(blob)
        print("  %-46s %5.1f s  %5.2f MB  %.2f:1 vs 16-bit"
              % (dst, len(d) / float(rate), len(blob) / 1048576.0,
                 len(d) * 2.0 / len(blob)))
        return
    if sys.argv[1] != "roundtrip":
        raise SystemExit(__doc__)
    src = sys.argv[2]
    w = wave.open(src, "rb")
    rate = w.getframerate()
    d = array.array("h")
    d.frombytes(w.readframes(w.getnframes()))
    enc = encode(d)
    dec = decode(enc)
    m = min(len(d), len(dec))
    err = sum((d[k] - dec[k]) ** 2 for k in range(m))
    sig = sum(d[k] * d[k] for k in range(m))
    snr = 10 * math.log10(sig / err) if err else 99.0
    print("  %-32s %7.1f KB -> %6.1f KB  %.2f:1   SNR %4.1f dB"
          % (src.replace("\\", "/").split("/")[-1],
             len(d) * 2 / 1024.0, len(enc) / 1024.0,
             len(d) * 2.0 / len(enc), snr))
    if len(sys.argv) > 3:
        wav_write(sys.argv[3], dec[:m], rate)


if __name__ == "__main__":
    main()
