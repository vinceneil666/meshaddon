// Hardware test of web/flash.js (the exact code the browser runs) from Node, with a Python serial bridge.
//   node firmware/test-flash.mjs /dev/ttyACM0 fw/meshaddon-heltec_v4-v1.18.0-addon0.1.bin [0x10000]
import { spawn } from "node:child_process";
import { readFileSync, copyFileSync, mkdtempSync } from "node:fs";
import { tmpdir } from "node:os";
import { join, dirname } from "node:path";
import { fileURLToPath } from "node:url";

const here = dirname(fileURLToPath(import.meta.url));
globalThis.crypto ??= (await import("node:crypto")).webcrypto;   // browsers have this built in
const [dev, file, addrArg] = process.argv.slice(2);
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

// load the browser modules as ESM
const tmp = mkdtempSync(join(process.env.TMPDIR || tmpdir(), "flashtest-"));
copyFileSync(join(here, "../web/flash.js"), join(tmp, "flash.mjs"));
copyFileSync(join(here, "../web/vendor/esptool-js-0.7.1.bundle.js"), join(tmp, "esptool.mjs"));
const { flashFirmware, sha256hex } = await import(join(tmp, "flash.mjs"));
const esptool = await import(join(tmp, "esptool.mjs"));

class BridgedPort {          // minimal Web Serial SerialPort stand-in
  getInfo() { return { usbVendorId: 0x303a, usbProductId: 0x1001 }; }
  async open({ baudRate }) {
    const p = (this.p = spawn("python3", [join(here, "serial-bridge.py"), dev, String(baudRate)], { stdio: ["pipe", "pipe", "inherit", "pipe"] }));
    this.ctrl = p.stdio[3];
    this.readable = new ReadableStream({
      start: (c) => { p.stdout.on("data", (d) => c.enqueue(new Uint8Array(d))); p.stdout.on("end", () => { try { c.close(); } catch (_) {} }); },
    });
    this.writable = new WritableStream({ write: (chunk) => new Promise((r) => p.stdin.write(Buffer.from(chunk), r)) });
    await sleep(400);
  }
  async setSignals(s) {
    this.ctrl.write(JSON.stringify({ dtr: s.dataTerminalReady ?? null, rts: s.requestToSend ?? null }) + "\n");
    await sleep(2);
  }
  async close() { this.p?.kill(); this.readable = null; this.writable = null; await sleep(300); }
}

const image = new Uint8Array(readFileSync(file));
const address = addrArg ? parseInt(addrArg) : 0x10000;
console.log(`image ${file} ${image.length} B sha256 ${await sha256hex(image)} -> 0x${address.toString(16)}`);
const t0 = Date.now(); let lastPct = -1;
const chip = await flashFirmware({
  esptool, port: new BridgedPort(), image, address,
  log: (s) => console.log("  esptool:", s),
  onProgress: (w, t) => { const pct = Math.floor((w / t) * 100); if (pct >= lastPct + 10) { lastPct = pct; console.log(`  ${pct}%`); } },
});
console.log(`DONE: ${chip}, ${((Date.now() - t0) / 1000).toFixed(1)} s`);
process.exit(0);
