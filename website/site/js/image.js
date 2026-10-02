// Reads the structure of ESP32 flash images: the partition table of a full
// (merged) image, the length of an app image, and MeshPunk's board tag.

import { md5hex } from './md5.js';

// The partition table occupies 0xC00 bytes at flash offset 0x8000.
export const PT_OFFSET = 0x8000;
export const PT_SIZE = 0xc00;

const PT_ENTRY_SIZE = 32;
const PT_MAGIC = 0x50aa;
const PT_MD5_MAGIC = 0xebeb;
const APP_TYPE = 0x00;
const SUBTYPE_OTA_0 = 0x10;

const APP_MAGIC = 0xe9;
const APP_HEADER_SIZE = 24;
const APP_MAX_SEGMENTS = 16;

// src/ota_tag.h: a main firmware image carries "MESHPUNK-BOARD:<slug>\0".
const BOARD_TAG = new TextEncoder().encode('MESHPUNK-BOARD:');
const BOARD_SLUG_MAX = 31;

export function hex(bytes) {
  let out = '';
  for (let i = 0; i < bytes.length; i++) out += bytes[i].toString(16).padStart(2, '0');
  return out;
}

/** @returns {Promise<string>} SHA-256 of bytes as lowercase hex */
export async function sha256hex(bytes) {
  return hex(new Uint8Array(await crypto.subtle.digest('SHA-256', bytes)));
}

/**
 * Parses a partition table (the PT_SIZE bytes found at PT_OFFSET).
 * @param {Uint8Array} table
 * @returns {{ok: true, entries: Array<{label: string, type: number, subtype: number, offset: number, size: number}>}
 *          | {ok: false, reason: string}}
 */
export function readPartitionTable(table) {
  const view = new DataView(table.buffer, table.byteOffset, table.byteLength);
  const entries = [];
  for (let pos = 0; pos + PT_ENTRY_SIZE <= table.length; pos += PT_ENTRY_SIZE) {
    const magic = view.getUint16(pos, true);
    if (magic === PT_MAGIC) {
      let label = '';
      for (let i = pos + 12; i < pos + 28 && table[i] !== 0; i++) label += String.fromCharCode(table[i]);
      entries.push({
        label,
        type: table[pos + 2],
        subtype: table[pos + 3],
        offset: view.getUint32(pos + 4, true),
        size: view.getUint32(pos + 8, true),
      });
      continue;
    }
    if (magic === PT_MD5_MAGIC) {
      if (md5hex(table.subarray(0, pos)) !== hex(table.subarray(pos + 16, pos + 32))) {
        return { ok: false, reason: 'the partition table checksum does not match its entries' };
      }
      if (entries.length === 0) return { ok: false, reason: 'the partition table is empty' };
      return { ok: true, entries };
    }
    break;
  }
  return { ok: false, reason: 'there is no partition table at 0x8000' };
}

/** The single ota_0 app partition of a table, or null when there is not exactly one. */
export function mainAppPartition(entries) {
  const found = entries.filter((e) => e.type === APP_TYPE && e.subtype === SUBTYPE_OTA_0);
  return found.length === 1 ? found[0] : null;
}

/**
 * Length an ESP app image declares through its header: the segments, the
 * checksum padded to 16 bytes, and the appended SHA-256 when flagged.
 * @returns {number|null} null when bytes do not start with an app image header
 */
export function appImageLength(bytes) {
  if (bytes.length < APP_HEADER_SIZE || bytes[0] !== APP_MAGIC) return null;
  const segments = bytes[1];
  if (segments === 0 || segments > APP_MAX_SEGMENTS) return null;
  const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  let pos = APP_HEADER_SIZE;
  for (let i = 0; i < segments; i++) {
    if (pos + 8 > bytes.length) return null;
    pos += 8 + view.getUint32(pos + 4, true);
    if (pos > bytes.length) return null;
  }
  pos = (pos + 16) & ~15;
  if (bytes[23] === 1) pos += 32;
  return pos;
}

/**
 * Slug of the first complete board tag in an image, or null.
 * @param {Uint8Array} bytes
 */
export function findBoardTag(bytes) {
  const first = BOARD_TAG[0];
  const last = bytes.length - BOARD_TAG.length;
  scan: for (let i = 0; i <= last; i++) {
    if (bytes[i] !== first) continue;
    for (let j = 1; j < BOARD_TAG.length; j++) {
      if (bytes[i + j] !== BOARD_TAG[j]) continue scan;
    }
    const start = i + BOARD_TAG.length;
    const limit = Math.min(bytes.length, start + BOARD_SLUG_MAX + 1);
    let end = start;
    while (end < limit && bytes[end] !== 0) end++;
    if (end === limit || end === start) continue;
    return String.fromCharCode(...bytes.subarray(start, end));
  }
  return null;
}

/**
 * Tells a full flash image from an app image.
 * @param {Uint8Array} bytes
 * @returns {{kind: 'full', entries: Array}|{kind: 'app'}|{kind: 'unknown', reason: string}}
 */
export function classifyImage(bytes) {
  if (bytes.length >= PT_OFFSET + PT_SIZE) {
    const table = readPartitionTable(bytes.subarray(PT_OFFSET, PT_OFFSET + PT_SIZE));
    if (table.ok) return { kind: 'full', entries: table.entries };
  }
  const declared = appImageLength(bytes);
  if (declared === bytes.length) return { kind: 'app' };
  if (declared !== null) {
    return { kind: 'unknown', reason: `its header describes ${declared} bytes but the file has ${bytes.length}` };
  }
  return { kind: 'unknown', reason: 'it is neither a full flash image nor an ESP app image' };
}
