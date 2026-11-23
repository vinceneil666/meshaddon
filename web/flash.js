// MeshAddon browser flashing (phase 3). Shared by index.html and firmware/test-flash.mjs.
// Uses esptool-js 0.7.1 (vendor/esptool-js-0.7.1.bundle.js, Apache-2.0).
//
//   flashFirmware({ esptool, port, image, address, eraseAll, onProgress, log })
//     esptool  : the esptool-js module ({ ESPLoader, Transport })
//     port     : a Web Serial SerialPort (closed); the same object the CLI used
//     image    : Uint8Array with the firmware
//     address  : 0x10000 for an app-only update (keeps identity/settings), 0x0 for a merged image
//     eraseAll : true = erase the whole flash first (new node identity!)

export const APP_OFFSET = 0x10000;
// baudrate stays 115200: on the S3 native USB (USB-Serial/JTAG) the baud rate does not limit speed,
// and a baud change makes esptool-js close/reopen the port, which can reset the chip mid-flash.

export async function flashFirmware({ esptool, port, image, address, eraseAll = false, baudrate = 115200, onProgress, log }) {
  if (!(image instanceof Uint8Array) || image.length < 1024) throw new Error("firmware image is empty");
  if (image[0] !== 0xe9) throw new Error("not an ESP32 firmware image (bad magic byte)");
  const { ESPLoader, Transport } = esptool;
  const say = (s) => log && log(String(s).replace(/\s+$/, ""));
  const transport = new Transport(port, false);
  const terminal = { clean() {}, writeLine: say, write: say };
  const loader = new ESPLoader({ transport, baudrate, terminal, debugLogging: false });
  let chip;
  try {
    chip = await loader.main("default_reset");      // reset into the ROM bootloader, sync, upload stub
    if (!/ESP32-S3/i.test(chip)) throw new Error(`this is a ${chip}, not an ESP32-S3 (Heltec V4)`);
    await loader.writeFlash({
      fileArray: [{ data: image, address }],
      flashMode: "keep", flashFreq: "keep", flashSize: "keep",
      eraseAll, compress: true,
      reportProgress: (_i, written, total) => onProgress && onProgress(written, total),
      calculateMD5Hash: md5hex,                     // stub verifies the written flash against this
    });
    say("Flash verified. Restarting the board…");
    await loader.after("hard_reset");
  } finally {
    try { await transport.disconnect(); } catch (_) {}
  }
  return chip;
}

export async function sha256hex(bytes) {
  const d = await crypto.subtle.digest("SHA-256", bytes);
  return [...new Uint8Array(d)].map((b) => b.toString(16).padStart(2, "0")).join("");
}

// Compact MD5 (RFC 1321) over a Uint8Array -> hex string. esptool-js uses it to verify the written flash.
export function md5hex(bytes) {
  const K = new Uint32Array(64), S = [7, 12, 17, 22, 5, 9, 14, 20, 4, 11, 16, 23, 6, 10, 15, 21];
  for (let i = 0; i < 64; i++) K[i] = Math.floor(Math.abs(Math.sin(i + 1)) * 2 ** 32) >>> 0;
  const len = bytes.length, nblk = ((len + 8) >>> 6) + 1, buf = new Uint8Array(nblk * 64);
  buf.set(bytes); buf[len] = 0x80;
  const dv = new DataView(buf.buffer);
  dv.setUint32(nblk * 64 - 8, (len * 8) >>> 0, true);
  dv.setUint32(nblk * 64 - 4, Math.floor(len / 0x20000000), true);
  let a0 = 0x67452301, b0 = 0xefcdab89, c0 = 0x98badcfe, d0 = 0x10325476;
  const M = new Uint32Array(16);
  for (let blk = 0; blk < nblk; blk++) {
    for (let j = 0; j < 16; j++) M[j] = dv.getUint32(blk * 64 + j * 4, true);
    let A = a0, B = b0, C = c0, D = d0;
    for (let i = 0; i < 64; i++) {
      let F, g;
      if (i < 16) { F = (B & C) | (~B & D); g = i; }
      else if (i < 32) { F = (D & B) | (~D & C); g = (5 * i + 1) % 16; }
      else if (i < 48) { F = B ^ C ^ D; g = (3 * i + 5) % 16; }
      else { F = C ^ (B | ~D); g = (7 * i) % 16; }
      F = (F + A + K[i] + M[g]) >>> 0;
      A = D; D = C; C = B;
      const s = S[(i >> 4) * 4 + (i % 4)];
      B = (B + ((F << s) | (F >>> (32 - s)))) >>> 0;
    }
    a0 = (a0 + A) >>> 0; b0 = (b0 + B) >>> 0; c0 = (c0 + C) >>> 0; d0 = (d0 + D) >>> 0;
  }
  const out = new DataView(new ArrayBuffer(16));
  [a0, b0, c0, d0].forEach((v, i) => out.setUint32(i * 4, v, true));
  return [...new Uint8Array(out.buffer)].map((b) => b.toString(16).padStart(2, "0")).join("");
}
