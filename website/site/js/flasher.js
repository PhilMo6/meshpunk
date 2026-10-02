// Downloads a release image and writes an image to a device through
// esptool-js. No DOM: the loader, fetch and the progress callbacks are passed
// in, so the sequence can be exercised without a device (website/tests/).

import { PT_OFFSET, PT_SIZE, hex, sha256hex } from './image.js';
import { md5hex } from './md5.js';

export const STAGES = ['download', 'connect', 'check', 'write', 'verify', 'restart'];

export class FlashError extends Error {
  /**
   * @param {string} stage one of STAGES
   * @param {string} message
   */
  constructor(stage, message) {
    super(message);
    this.name = 'FlashError';
    this.stage = stage;
  }
}

const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));

function megabytes(bytes) {
  return `${(bytes / 1048576).toFixed(1)} MB`;
}

/**
 * Fetches a mirrored release file and checks it against the index.
 * @param {{path: string, size: number, sha256: string}} file entry from firmware/index.json
 * @param {URL|string} siteRoot URL the path is relative to
 * @param {{fetch: typeof fetch, onProgress: (fraction: number) => void}} io
 * @returns {Promise<Uint8Array>}
 */
export async function downloadImage(file, siteRoot, io) {
  const url = new URL(file.path, siteRoot);
  const response = await io.fetch(url, { cache: 'no-store' });
  if (!response.ok) {
    throw new FlashError('download', `The server answered HTTP ${response.status} for ${file.path}.`);
  }
  const bytes = new Uint8Array(file.size);
  const reader = response.body.getReader();
  let received = 0;
  for (;;) {
    const { done, value } = await reader.read();
    if (done) break;
    if (received + value.length > file.size) {
      throw new FlashError('download', `${file.path} is larger than the ${file.size} bytes the index lists.`);
    }
    bytes.set(value, received);
    received += value.length;
    io.onProgress(received / file.size);
  }
  if (received !== file.size) {
    throw new FlashError('download', `${file.path} ended after ${received} of ${file.size} bytes.`);
  }
  if ((await sha256hex(bytes)) !== file.sha256) {
    throw new FlashError('download', `${file.path} does not match its published SHA-256. Reload the page and try again.`);
  }
  return bytes;
}

async function flashCapacity(loader) {
  const id = await loader.readFlashId();
  const label = loader.DETECTED_FLASH_SIZES[(id >> 16) & 0xff];
  if (!label) {
    throw new FlashError(
      'check',
      `The flash chip (ID 0x${id.toString(16)}) is not one esptool-js knows the size of, so the image cannot be confirmed to fit. Nothing was written.`,
    );
  }
  return { label, bytes: loader.flashSizeBytes(label) };
}

// After the data of a flash read, the stub sends one more frame: the MD5 of
// that data (esptool.py's read_flash checks it). esptool-js 0.6.1's readFlash
// returns without reading that frame.
async function readFlashVerified(loader, transport, address, size) {
  const data = await loader.readFlash(address, size);
  const digest = await transport.read(loader.DEFAULT_TIMEOUT);
  if (digest.length !== 16 || hex(digest) !== md5hex(data)) {
    throw new FlashError(
      'check',
      'The partition table read from the device does not match the checksum the device sent with it. Nothing was written.',
    );
  }
  return data;
}

// esptool.py's hard reset is RTS high, 100 ms, RTS low. esptool-js 0.6.1's
// after('hard_reset') only sets RTS low.
async function restartDevice(loader, transport) {
  await loader.after('hard_reset');
  await transport.setRTS(true);
  await sleep(100);
  await transport.setRTS(false);
}

/**
 * Connects to the device behind a loader, checks that the image belongs
 * there, writes it, verifies it and restarts the device.
 *
 * kind 'full': the image is a whole flash image and goes to offset 0.
 * kind 'app':  the image goes to the app partition, and only when the
 *              device's partition table hashes to one of job.layouts.
 *
 * @param {{
 *   image: Uint8Array,
 *   kind: 'full'|'app',
 *   chip: string,
 *   boardName: string,
 *   layouts?: Array<{tableSha256: string, appOffset: number, appSize: number}>,
 * }} job
 * @param {{
 *   createLoader: (terminal: object) => {loader: object, transport: object},
 *   onStage: (stage: string) => void,
 *   onProgress: (fraction: number) => void,
 *   log: (text: string, endLine: boolean) => void,
 * }} io
 */
export async function flashDevice(job, io) {
  const terminal = {
    clean() {},
    writeLine: (text) => io.log(text, true),
    write: (text) => io.log(text, false),
  };
  const { loader, transport } = io.createLoader(terminal);

  let stage = 'connect';
  const enter = (next) => {
    stage = next;
    io.onStage(next);
  };

  let failure = null;
  try {
    enter('connect');
    await loader.main();

    enter('check');
    const chip = loader.chip.CHIP_NAME;
    if (chip !== job.chip) {
      throw new FlashError('check', `The connected chip is an ${chip}; ${job.boardName} has an ${job.chip}. Nothing was written.`);
    }
    let address = 0;
    if (job.kind === 'full') {
      const capacity = await flashCapacity(loader);
      if (capacity.bytes < job.image.length) {
        throw new FlashError(
          'check',
          `This device has ${capacity.label} of flash; the image is ${megabytes(job.image.length)}. Nothing was written.`,
        );
      }
    } else {
      const table = await readFlashVerified(loader, transport, PT_OFFSET, PT_SIZE);
      const digest = await sha256hex(table);
      const layout = job.layouts.find((candidate) => candidate.tableSha256 === digest);
      if (!layout) {
        throw new FlashError(
          'check',
          "This device's flash layout is not the one this release uses, so an update cannot be written safely. " +
            'Nothing was written. Use Fresh install (it erases the device), or, if MeshPunk was installed through ' +
            'the Launcher, update it from the Launcher.',
        );
      }
      if (job.image.length > layout.appSize) {
        throw new FlashError(
          'check',
          `The image is ${megabytes(job.image.length)}; the app partition holds ${megabytes(layout.appSize)}. Nothing was written.`,
        );
      }
      address = layout.appOffset;
    }

    enter('write');
    await loader.writeFlash({
      fileArray: [{ data: job.image, address }],
      flashSize: 'keep',
      flashMode: 'keep',
      flashFreq: 'keep',
      eraseAll: false,
      compress: true,
      reportProgress: (_fileIndex, written, total) => {
        io.onProgress(written / total);
        if (written === total) enter('verify');
      },
      calculateMD5Hash: (image) => md5hex(image),
    });

    enter('restart');
    await restartDevice(loader, transport);
  } catch (error) {
    failure = error instanceof FlashError ? error : new FlashError(stage, error?.message || String(error));
  }

  // After a connection the device is in download mode: its screen stays dark
  // until it is reset. A device that never connected is left as it is.
  if (failure && failure.stage !== 'connect' && failure.stage !== 'restart') {
    try {
      await restartDevice(loader, transport);
    } catch (error) {
      io.log(`The device could not be restarted from here: ${error?.message || error}`, true);
    }
  }
  try {
    await transport.disconnect();
  } catch (error) {
    io.log(`Closing the serial port: ${error?.message || error}`, true);
  }
  if (failure) throw failure;
}
