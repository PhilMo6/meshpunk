// node --test website/tests/
//
// Covers the flasher's pure logic: MD5, image inspection, the download check
// and the device sequence (against a scripted loader, no hardware). When a
// built site is present in website/_site, the JavaScript readers are also
// compared with what build_site.py wrote into firmware/index.json.

import assert from 'node:assert/strict';
import { createHash } from 'node:crypto';
import { existsSync, readFileSync } from 'node:fs';
import { dirname, join } from 'node:path';
import test from 'node:test';
import { fileURLToPath } from 'node:url';

import { FlashError, downloadImage, flashDevice } from '../site/js/flasher.js';
import {
  PT_OFFSET,
  PT_SIZE,
  appImageLength,
  classifyImage,
  findBoardTag,
  mainAppPartition,
  readPartitionTable,
  sha256hex,
} from '../site/js/image.js';
import { md5hex } from '../site/js/md5.js';

const HERE = dirname(fileURLToPath(import.meta.url));
const SITE = join(HERE, '..', '_site');

const nodeHash = (algorithm, bytes) => createHash(algorithm).update(bytes).digest('hex');

// ---- synthetic images ------------------------------------------------------

function partitionTable(entries, { corruptMd5 = false } = {}) {
  const table = new Uint8Array(PT_SIZE).fill(0xff);
  const view = new DataView(table.buffer);
  let pos = 0;
  for (const [label, type, subtype, offset, size] of entries) {
    view.setUint16(pos, 0x50aa, true);
    table[pos + 2] = type;
    table[pos + 3] = subtype;
    view.setUint32(pos + 4, offset, true);
    view.setUint32(pos + 8, size, true);
    table.fill(0, pos + 12, pos + 32);
    table.set(new TextEncoder().encode(label), pos + 12);
    pos += 32;
  }
  view.setUint16(pos, 0xebeb, true);
  const md5 = createHash('md5').update(table.subarray(0, pos)).digest();
  if (corruptMd5) md5[0] ^= 0xff;
  table.set(md5, pos + 16);
  return table;
}

const MESHPUNK_LAYOUT = [
  ['nvs', 1, 0x02, 0x9000, 0x5000],
  ['otadata', 1, 0x00, 0xe000, 0x2000],
  ['main', 0, 0x10, 0x10000, 0x580000],
  ['updater', 0, 0x00, 0x590000, 0x80000],
  ['assets', 1, 0x82, 0x610000, 0x9f0000],
];

function appImage(payloadLength, { tag = 'tdeck', hashAppended = true } = {}) {
  const payload = new Uint8Array(payloadLength);
  for (let i = 0; i < payload.length; i++) payload[i] = (i * 31) & 0xff;
  if (tag !== null) payload.set(new TextEncoder().encode(`MESHPUNK-BOARD:${tag}\0`), 16);
  let length = 24 + 8 + payload.length;
  length = (length + 16) & ~15;
  if (hashAppended) length += 32;
  const image = new Uint8Array(length);
  image[0] = 0xe9;
  image[1] = 1;
  image[23] = hashAppended ? 1 : 0;
  new DataView(image.buffer).setUint32(28, payload.length, true);
  image.set(payload, 32);
  return image;
}

function fullImage(app, table = partitionTable(MESHPUNK_LAYOUT)) {
  const image = new Uint8Array(0x10000 + app.length).fill(0xff);
  image[0] = 0xe9;
  image.set(table, PT_OFFSET);
  image.set(app, 0x10000);
  return image;
}

// ---- md5 --------------------------------------------------------------------

test('md5hex matches node:crypto across block boundaries', () => {
  assert.equal(md5hex(new Uint8Array(0)), 'd41d8cd98f00b204e9800998ecf8427e');
  assert.equal(md5hex(new TextEncoder().encode('abc')), '900150983cd24fb0d6963f7d28e17f72');
  for (const length of [1, 55, 56, 57, 63, 64, 65, 119, 120, 127, 128, 1000, 65536, 1048577]) {
    const bytes = new Uint8Array(length);
    for (let i = 0; i < length; i++) bytes[i] = (i * 131 + (i >>> 8)) & 0xff;
    assert.equal(md5hex(bytes), nodeHash('md5', bytes), `length ${length}`);
  }
});

test('md5hex honours a subarray offset', () => {
  const backing = new Uint8Array(300).map((_, i) => i & 0xff);
  const part = backing.subarray(7, 207);
  assert.equal(md5hex(part), nodeHash('md5', part));
});

// ---- image inspection -------------------------------------------------------

test('readPartitionTable reads entries and checks the MD5 entry', () => {
  const table = readPartitionTable(partitionTable(MESHPUNK_LAYOUT));
  assert.equal(table.ok, true);
  assert.deepEqual(
    table.entries.map((e) => [e.label, e.offset, e.size]),
    MESHPUNK_LAYOUT.map(([label, , , offset, size]) => [label, offset, size]),
  );
  assert.deepEqual(mainAppPartition(table.entries), {
    label: 'main', type: 0, subtype: 0x10, offset: 0x10000, size: 0x580000,
  });

  assert.equal(readPartitionTable(partitionTable(MESHPUNK_LAYOUT, { corruptMd5: true })).ok, false);
  assert.equal(readPartitionTable(new Uint8Array(PT_SIZE).fill(0xff)).ok, false);
  assert.equal(readPartitionTable(partitionTable([])).ok, false);
});

test('mainAppPartition wants exactly one ota_0', () => {
  const none = readPartitionTable(partitionTable([['app0', 0, 0x00, 0x10000, 0x580000]]));
  assert.equal(mainAppPartition(none.entries), null);
  const two = readPartitionTable(partitionTable([
    ['a', 0, 0x10, 0x10000, 0x100000],
    ['b', 0, 0x10, 0x110000, 0x100000],
  ]));
  assert.equal(mainAppPartition(two.entries), null);
});

test('appImageLength follows the header', () => {
  for (const hashAppended of [true, false]) {
    for (const payload of [100, 101, 115, 116, 4096]) {
      const image = appImage(payload, { hashAppended });
      assert.equal(appImageLength(image), image.length);
    }
  }
  assert.equal(appImageLength(new Uint8Array(64)), null);
  assert.equal(appImageLength(new Uint8Array([0xe9, 0])), null);
  const truncated = appImage(4096).subarray(0, 2000);
  assert.equal(appImageLength(truncated), null);
});

test('classifyImage tells full, app and other files apart', () => {
  const app = appImage(5000);
  assert.equal(classifyImage(app).kind, 'app');
  assert.equal(classifyImage(fullImage(app)).kind, 'full');

  const padded = new Uint8Array(app.length + 16);
  padded.set(app);
  assert.equal(classifyImage(padded).kind, 'unknown');
  assert.equal(classifyImage(new Uint8Array(70000)).kind, 'unknown');
  assert.equal(classifyImage(fullImage(app, partitionTable(MESHPUNK_LAYOUT, { corruptMd5: true }))).kind, 'unknown');
});

test('findBoardTag returns the first complete tag', () => {
  assert.equal(findBoardTag(appImage(500, { tag: 'heltec_v4' })), 'heltec_v4');
  assert.equal(findBoardTag(appImage(500, { tag: null })), null);

  const encode = (text) => new TextEncoder().encode(text);
  assert.equal(findBoardTag(encode('xxMESHPUNK-BOARD:\0yyMESHPUNK-BOARD:wio_l2\0')), 'wio_l2');
  assert.equal(findBoardTag(encode('MESHPUNK-BOARD:unterminated')), null);
  assert.equal(findBoardTag(encode(`MESHPUNK-BOARD:${'a'.repeat(40)}\0`)), null);
  assert.equal(findBoardTag(encode(`MESHPUNK-BOARD:${'a'.repeat(31)}\0`)), 'a'.repeat(31));
});

// ---- download ---------------------------------------------------------------

async function fileEntry(bytes) {
  return { path: 'firmware/stable/v1/x.bin', size: bytes.length, sha256: await sha256hex(bytes) };
}

test('downloadImage returns verified bytes and reports progress', async () => {
  const bytes = appImage(9000);
  const seen = [];
  const got = await downloadImage(await fileEntry(bytes), 'https://example.test/site/', {
    fetch: async (url, options) => {
      assert.equal(String(url), 'https://example.test/site/firmware/stable/v1/x.bin');
      assert.equal(options.cache, 'no-store');
      return new Response(bytes);
    },
    onProgress: (fraction) => seen.push(fraction),
  });
  assert.deepEqual(got, bytes);
  assert.equal(seen.at(-1), 1);
});

test('downloadImage refuses HTTP errors, wrong sizes and wrong hashes', async () => {
  const bytes = appImage(9000);
  const entry = await fileEntry(bytes);
  const io = (response) => ({ fetch: async () => response, onProgress() {} });
  const stage = { name: 'FlashError', stage: 'download' };

  await assert.rejects(downloadImage(entry, 'https://x.test/', io(new Response('no', { status: 404 }))), stage);
  await assert.rejects(downloadImage(entry, 'https://x.test/', io(new Response(bytes.subarray(0, 100)))), stage);
  const longer = new Uint8Array(bytes.length + 1);
  await assert.rejects(downloadImage(entry, 'https://x.test/', io(new Response(longer))), stage);
  const altered = bytes.slice();
  altered[500] ^= 1;
  await assert.rejects(downloadImage(entry, 'https://x.test/', io(new Response(altered))), stage);
});

// ---- device sequence --------------------------------------------------------

// Winbond W25Q128: JEDEC ID bytes EF 40 18, capacity code 0x18 = 16 MB.
const FLASH_16MB = 0x1840ef;
const FLASH_4MB = 0x1640ef;

function scriptedDevice(script = {}) {
  const calls = [];
  const transport = {
    setRTS: async (level) => { calls.push(`rts:${level}`); },
    disconnect: async () => { calls.push('disconnect'); },
    // The frame the stub sends after the data of a flash read: its MD5.
    read: async (timeout) => {
      calls.push(`readDigest:${timeout}`);
      return script.digest ?? createHash('md5').update(script.table).digest();
    },
  };
  const loader = {
    DEFAULT_TIMEOUT: 3000,
    chip: { CHIP_NAME: script.chip ?? 'ESP32-S3' },
    DETECTED_FLASH_SIZES: { 0x16: '4MB', 0x18: '16MB' },
    flashSizeBytes: (label) => parseInt(label, 10) * 1048576,
    main: async () => {
      calls.push('main');
      if (script.failConnect) throw new Error('Failed to connect with the device');
    },
    readFlashId: async () => { calls.push('readFlashId'); return script.flashId ?? FLASH_16MB; },
    readFlash: async (address, size) => { calls.push(`readFlash:${address}:${size}`); return script.table; },
    writeFlash: async (options) => {
      const [file] = options.fileArray;
      calls.push(`write:${file.address}:${file.data.length}`);
      script.onWrite?.(options);
      options.reportProgress(0, 0, 1000);
      if (script.failWrite) throw new Error('Failed to write compressed data to flash after seq 3');
      options.reportProgress(0, 1000, 1000);
      if (script.md5Mismatch) throw new Error('MD5 of file does not match data in flash!');
    },
    after: async (mode) => { calls.push(`after:${mode}`); },
  };
  const stages = [];
  const progress = [];
  const lines = [];
  const io = {
    createLoader: (terminal) => {
      terminal.clean();
      terminal.writeLine('esptool.js');
      return { loader, transport };
    },
    onStage: (stage) => stages.push(stage),
    onProgress: (fraction) => progress.push(fraction),
    log: (text) => lines.push(text),
  };
  return { calls, stages, progress, lines, io };
}

const RESTART = ['after:hard_reset', 'rts:true', 'rts:false'];
const READ_TABLE = [`readFlash:${PT_OFFSET}:${PT_SIZE}`, 'readDigest:3000'];
const board = { chip: 'ESP32-S3', boardName: 'LilyGo T-Deck' };

async function layoutFor(table) {
  return { tableSha256: await sha256hex(table), appOffset: 0x10000, appSize: 0x580000 };
}

test('fresh install writes the full image at 0, verifies and restarts', async () => {
  const image = fullImage(appImage(3000));
  let options;
  const device = scriptedDevice({ onWrite: (o) => { options = o; } });
  await flashDevice({ ...board, image, kind: 'full' }, device.io);

  assert.deepEqual(device.calls, ['main', 'readFlashId', `write:0:${image.length}`, ...RESTART, 'disconnect']);
  assert.deepEqual(device.stages, ['connect', 'check', 'write', 'verify', 'restart']);
  assert.equal(options.compress, true);
  assert.equal(options.eraseAll, false);
  assert.deepEqual([options.flashSize, options.flashMode, options.flashFreq], ['keep', 'keep', 'keep']);
  assert.equal(options.calculateMD5Hash(image), nodeHash('md5', image));
  assert.equal(device.progress.at(-1), 1);
  assert.deepEqual(device.lines, ['esptool.js']);
});

test('update writes the app at the offset of the matching layout', async () => {
  const table = partitionTable(MESHPUNK_LAYOUT);
  const image = appImage(3000);
  const other = { tableSha256: 'f'.repeat(64), appOffset: 0x20000, appSize: 0x100000 };
  const device = scriptedDevice({ table });
  await flashDevice({ ...board, image, kind: 'app', layouts: [other, await layoutFor(table)] }, device.io);

  assert.deepEqual(device.calls, ['main', ...READ_TABLE, `write:${0x10000}:${image.length}`, ...RESTART, 'disconnect']);
});

test('update is refused when the table read fails the checksum the stub sends', async () => {
  const table = partitionTable(MESHPUNK_LAYOUT);
  const device = scriptedDevice({ table, digest: new Uint8Array(16) });
  await assert.rejects(
    flashDevice({ ...board, image: appImage(3000), kind: 'app', layouts: [await layoutFor(table)] }, device.io),
    { stage: 'check', message: /checksum/ },
  );
  assert.deepEqual(device.calls, ['main', ...READ_TABLE, ...RESTART, 'disconnect']);
});

test('update is refused on a different partition table, and the device is restarted', async () => {
  const launcher = partitionTable([
    ['nvs', 1, 0x02, 0x9000, 0x5000],
    ['test', 0, 0x20, 0x10000, 0x180000],
    ['app0', 0, 0x10, 0x1a0000, 0x480000],
  ]);
  const device = scriptedDevice({ table: launcher });
  const job = { ...board, image: appImage(3000), kind: 'app', layouts: [await layoutFor(partitionTable(MESHPUNK_LAYOUT))] };

  await assert.rejects(flashDevice(job, device.io), (error) => {
    assert.ok(error instanceof FlashError);
    assert.equal(error.stage, 'check');
    assert.match(error.message, /Nothing was written/);
    return true;
  });
  assert.deepEqual(device.calls, ['main', ...READ_TABLE, ...RESTART, 'disconnect']);
});

test('update is refused when the app does not fit the partition', async () => {
  const table = partitionTable(MESHPUNK_LAYOUT);
  const device = scriptedDevice({ table });
  const layout = { ...(await layoutFor(table)), appSize: 1024 };
  await assert.rejects(
    flashDevice({ ...board, image: appImage(3000), kind: 'app', layouts: [layout] }, device.io),
    { stage: 'check' },
  );
  assert.ok(!device.calls.some((call) => call.startsWith('write')));
});

test('a different chip is refused before anything is read or written', async () => {
  const device = scriptedDevice({ chip: 'ESP32-C3' });
  await assert.rejects(
    flashDevice({ ...board, image: fullImage(appImage(3000)), kind: 'full' }, device.io),
    { stage: 'check', message: /ESP32-C3/ },
  );
  assert.deepEqual(device.calls, ['main', ...RESTART, 'disconnect']);
});

test('a full image larger than the flash is refused', async () => {
  const image = new Uint8Array(5 * 1048576);
  const device = scriptedDevice({ flashId: FLASH_4MB });
  await assert.rejects(flashDevice({ ...board, image, kind: 'full' }, device.io), { stage: 'check', message: /4MB/ });
  assert.deepEqual(device.calls, ['main', 'readFlashId', ...RESTART, 'disconnect']);
});

test('an unknown flash ID is refused', async () => {
  const device = scriptedDevice({ flashId: 0x7f40ef });
  await assert.rejects(
    flashDevice({ ...board, image: fullImage(appImage(3000)), kind: 'full' }, device.io),
    { stage: 'check', message: /0x7f40ef/ },
  );
});

test('a connect failure leaves the device alone and closes the port', async () => {
  const device = scriptedDevice({ failConnect: true });
  await assert.rejects(
    flashDevice({ ...board, image: fullImage(appImage(3000)), kind: 'full' }, device.io),
    { stage: 'connect', message: 'Failed to connect with the device' },
  );
  assert.deepEqual(device.calls, ['main', 'disconnect']);
});

test('a write failure is reported at the write stage and the device is restarted', async () => {
  const device = scriptedDevice({ failWrite: true });
  await assert.rejects(
    flashDevice({ ...board, image: fullImage(appImage(3000)), kind: 'full' }, device.io),
    { stage: 'write', message: /seq 3/ },
  );
  assert.deepEqual(device.calls.slice(-4), [...RESTART, 'disconnect']);
});

test('an MD5 mismatch is reported at the verify stage', async () => {
  const device = scriptedDevice({ md5Mismatch: true });
  await assert.rejects(
    flashDevice({ ...board, image: fullImage(appImage(3000)), kind: 'full' }, device.io),
    { stage: 'verify', message: /MD5/ },
  );
  assert.deepEqual(device.stages, ['connect', 'check', 'write', 'verify']);
});

// ---- the built site, when present ---------------------------------------------

const indexPath = join(SITE, 'firmware', 'index.json');

test('JavaScript readers agree with firmware/index.json', { skip: !existsSync(indexPath) && 'no built site' }, async () => {
  const index = JSON.parse(readFileSync(indexPath, 'utf8'));
  let checked = 0;
  for (const channel of Object.values(index.channels)) {
    if (!channel.release) continue;
    for (const [slug, entry] of Object.entries(channel.release.boards)) {
      const merged = new Uint8Array(readFileSync(join(SITE, entry.merged.path)));
      const firmware = new Uint8Array(readFileSync(join(SITE, entry.firmware.path)));

      assert.equal(await sha256hex(merged), entry.merged.sha256);
      assert.equal(await sha256hex(firmware), entry.firmware.sha256);
      assert.equal(md5hex(firmware), nodeHash('md5', firmware));

      const full = classifyImage(merged);
      assert.equal(full.kind, 'full', entry.merged.path);
      assert.equal(classifyImage(firmware).kind, 'app', entry.firmware.path);

      const app = mainAppPartition(full.entries);
      assert.equal(app.offset, entry.app_offset);
      assert.equal(app.size, entry.app_size);
      assert.equal(await sha256hex(merged.subarray(PT_OFFSET, PT_OFFSET + PT_SIZE)), entry.table_sha256);

      assert.equal(findBoardTag(firmware), slug);
      assert.equal(findBoardTag(merged), slug);
      assert.ok(index.boards[slug], `${slug} is in the board catalog`);
      checked++;
    }
  }
  assert.ok(checked > 0);
});
