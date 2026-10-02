// MD5 (RFC 1321). WebCrypto has no MD5, and the ESP flasher stub reports the
// MD5 of what it wrote, so the post-write check needs one.

const SHIFTS = [
  7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
  5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20,
  4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
  6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21,
];

const K = new Int32Array(64);
for (let i = 0; i < 64; i++) {
  K[i] = Math.floor(Math.abs(Math.sin(i + 1)) * 0x100000000) | 0;
}

function compress(state, view, offset, m) {
  for (let i = 0; i < 16; i++) m[i] = view.getInt32(offset + i * 4, true);
  let a = state[0];
  let b = state[1];
  let c = state[2];
  let d = state[3];
  for (let i = 0; i < 64; i++) {
    let f;
    let g;
    if (i < 16) {
      f = (b & c) | (~b & d);
      g = i;
    } else if (i < 32) {
      f = (d & b) | (~d & c);
      g = (5 * i + 1) & 15;
    } else if (i < 48) {
      f = b ^ c ^ d;
      g = (3 * i + 5) & 15;
    } else {
      f = c ^ (b | ~d);
      g = (7 * i) & 15;
    }
    f = (f + a + K[i] + m[g]) | 0;
    a = d;
    d = c;
    c = b;
    const s = SHIFTS[i];
    b = (b + ((f << s) | (f >>> (32 - s)))) | 0;
  }
  state[0] = (state[0] + a) | 0;
  state[1] = (state[1] + b) | 0;
  state[2] = (state[2] + c) | 0;
  state[3] = (state[3] + d) | 0;
}

/**
 * @param {Uint8Array} bytes
 * @returns {string} 32 lowercase hex characters
 */
export function md5hex(bytes) {
  const state = new Int32Array([0x67452301, 0xefcdab89 | 0, 0x98badcfe | 0, 0x10325476]);
  const m = new Int32Array(16);
  const length = bytes.length;
  const whole = length - (length % 64);

  const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  for (let offset = 0; offset < whole; offset += 64) compress(state, view, offset, m);

  // Final block(s): the remaining bytes, 0x80, zeros, then the bit length as
  // a 64-bit little-endian integer.
  const rest = length - whole;
  const tail = new Uint8Array(rest < 56 ? 64 : 128);
  tail.set(bytes.subarray(whole));
  tail[rest] = 0x80;
  const tailView = new DataView(tail.buffer);
  tailView.setUint32(tail.length - 8, (length << 3) >>> 0, true);
  tailView.setUint32(tail.length - 4, Math.floor(length / 0x20000000), true);
  for (let offset = 0; offset < tail.length; offset += 64) compress(state, tailView, offset, m);

  const out = new DataView(new ArrayBuffer(16));
  for (let i = 0; i < 4; i++) out.setInt32(i * 4, state[i], true);
  let hex = '';
  for (let i = 0; i < 16; i++) hex += out.getUint8(i).toString(16).padStart(2, '0');
  return hex;
}
