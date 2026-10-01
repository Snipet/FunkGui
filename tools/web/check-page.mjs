#!/usr/bin/env node
// tools/web/check-page.mjs <dir>: runs FunkGui::web's browser check (test/web/page.cpp) in headless Chrome and reports
// its verdict (v0.12.0; FCompressor ADR-93, docs/sprints/web-b.md G-C). <dir> is the page as the `web` preset built it:
// <build-web>/test/web, holding index.html, funkgui_web_page.js and funkgui_web_page.wasm. CTest runs it as
// fg.web.page (labels fg;live: never in `verify`, which has no browser):
//
//   cmake --workflow --preset web-verify && tools/web/check-page.mjs build-web/test/web
//   ctest --test-dir build-web -L live --output-on-failure
//
// What it does: serves <dir> with `python3 -m http.server` on 127.0.0.1 (a free port), starts Chrome headless with its
// own throwaway profile, opens index.html through the DevTools protocol, waits until document.title is "PASS" or
// starts with "FAIL", prints the page's log (its <pre>), and stops Chrome and the server whatever happened.
// Exit code: 0 the page said PASS; 1 it said FAIL; 2 the check could not run (no Chrome, no python3, no page, no
// verdict within the timeout). Nothing is installed or downloaded; node (which Emscripten needs anyway) is the only
// interpreter besides python3.
//
//   --chrome <path>       the browser (default: $CHROME, else Google Chrome's usual place on macOS, else
//                         google-chrome, chromium or chromium-browser on PATH)
//   --chrome-flag <flag>  an extra Chrome flag (repeatable). Without any, the GPU path is ANGLE on Metal on macOS and
//                         SwiftShader elsewhere (--use-angle=swiftshader --enable-unsafe-swiftshader): WebGL2 either
//                         way. Giving one replaces that default.
//   --timeout <seconds>   how long to wait for the verdict (default 90)
//
// By hand instead: python3 -m http.server 8137 --bind 127.0.0.1 --directory <dir>, then open
// http://127.0.0.1:8137/index.html in any browser and read the tab's title.

import { spawn, spawnSync } from 'node:child_process';
import { existsSync, mkdtempSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join, resolve } from 'node:path';

const usage = 'usage: tools/web/check-page.mjs <dir with index.html> [--chrome <path>] [--chrome-flag <flag>]... '
            + '[--timeout <seconds>]';

function refuse(why) {
  console.error(`check-page: ${why}`);
  process.exit(2);
}

// ---- arguments ------------------------------------------------------------------------------------------------------
let dir = '', chrome = process.env.CHROME || '', timeoutS = 90;
const flags = [];
for (let i = 2; i < process.argv.length; ++i) {
  const a = process.argv[i];
  if (a === '--chrome') chrome = process.argv[++i] || '';
  else if (a === '--chrome-flag') flags.push(process.argv[++i] || '');
  else if (a === '--timeout') timeoutS = Number(process.argv[++i]);
  else if (a.startsWith('-') || dir) refuse(usage);
  else dir = a;
}
if (!dir || !(timeoutS > 0)) refuse(usage);
dir = resolve(dir);
for (const f of ['index.html', 'funkgui_web_page.js', 'funkgui_web_page.wasm'])
  if (!existsSync(join(dir, f)))
    refuse(`${dir} has no ${f}: build the web preset first (cmake --workflow --preset web-verify)`);

if (!chrome) {
  const mac = '/Applications/Google Chrome.app/Contents/MacOS/Google Chrome';
  if (process.platform === 'darwin' && existsSync(mac)) chrome = mac;
  for (const name of ['google-chrome', 'chromium', 'chromium-browser'])
    if (!chrome && spawnSync('which', [name]).status === 0) chrome = name;
}
if (!chrome) refuse('no Chrome found: pass --chrome <path> or set CHROME');
if (flags.length === 0)
  flags.push(...(process.platform === 'darwin' ? ['--use-angle=metal']
                                               : ['--use-angle=swiftshader', '--enable-unsafe-swiftshader']));

// ---- the server, the browser, and their end -------------------------------------------------------------------------
const children = [];
let profile = '';
function cleanUp() {
  for (const c of children) {
    try { c.kill('SIGKILL'); } catch { /* already gone */ }
  }
  if (profile) {
    try {
      rmSync(profile, { recursive: true, force: true, maxRetries: 5, retryDelay: 100 });
    } catch { /* best effort */ }
  }
}
function finish(code) {
  cleanUp();
  process.exit(code);
}
process.on('SIGINT', () => finish(2));
process.on('SIGTERM', () => finish(2));

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const deadline = Date.now() + timeoutS * 1000;

// A line a child prints that matches `pattern` (on either stream), or null at the deadline or when it exits.
function lineFrom(child, pattern) {
  return new Promise((done) => {
    let seen = '';
    const look = (chunk) => {
      seen += chunk;
      const m = seen.match(pattern);
      if (m) done(m);
    };
    child.stdout.on('data', look);
    child.stderr.on('data', look);
    child.on('error', () => done(null));
    child.on('exit', () => done(null));
    setTimeout(() => done(null), Math.max(0, deadline - Date.now()));
  });
}

async function main() {
  // python prints "Serving HTTP on 127.0.0.1 port N" once it listens; port 0 asks the system for a free one.
  const server = spawn('python3', ['-u', '-m', 'http.server', '0', '--bind', '127.0.0.1', '--directory', dir],
                       { stdio: ['ignore', 'pipe', 'pipe'] });
  children.push(server);
  const serving = await lineFrom(server, /Serving HTTP on 127\.0\.0\.1 port (\d+)/);
  if (!serving) refuseRunning('python3 -m http.server did not start');
  const url = `http://127.0.0.1:${serving[1]}/index.html`;

  profile = mkdtempSync(join(tmpdir(), 'funkgui-web-page-'));
  const browser = spawn(chrome, ['--headless=new', '--remote-debugging-port=0', `--user-data-dir=${profile}`,
                                 '--no-first-run', '--no-default-browser-check', '--disable-extensions', ...flags,
                                 'about:blank'], { stdio: ['ignore', 'pipe', 'pipe'] });
  children.push(browser);
  const listening = await lineFrom(browser, /DevTools listening on (ws:\/\/[^\s]+)/);
  if (!listening) refuseRunning(`${chrome} did not start with a DevTools port`);

  // The DevTools protocol over the browser's WebSocket: one flat session attached to the page's target.
  const socket = new WebSocket(listening[1]);
  await new Promise((open, bad) => {
    socket.addEventListener('open', open, { once: true });
    socket.addEventListener('error', () => bad(new Error('the DevTools socket did not open')), { once: true });
  });
  let nextId = 0;
  const waiting = new Map();
  socket.addEventListener('message', (event) => {
    const message = JSON.parse(event.data);
    if (message.id !== undefined && waiting.has(message.id)) {
      const { answer, reject } = waiting.get(message.id);
      waiting.delete(message.id);
      if (message.error) reject(new Error(message.error.message));
      else answer(message.result);
    }
  });
  const send = (method, params = {}, sessionId = undefined) => new Promise((answer, reject) => {
    const id = ++nextId;
    waiting.set(id, { answer, reject });
    socket.send(JSON.stringify({ id, method, params, sessionId }));
  });
  const { targetId } = await send('Target.createTarget', { url });
  const { sessionId } = await send('Target.attachToTarget', { targetId, flatten: true });
  const evaluate = async (expression) => {
    const { result } = await send('Runtime.evaluate', { expression, returnByValue: true }, sessionId);
    return result.value;
  };

  console.log(`check-page: ${url} in ${chrome} (${flags.join(' ')})`);
  let title = '';
  while (Date.now() < deadline) {
    title = String(await evaluate('document.title'));
    if (title === 'PASS' || title.startsWith('FAIL')) break;
    await sleep(100);
  }
  const log = await evaluate("(document.getElementById('funkgui-log') || {}).textContent || ''");
  process.stdout.write(String(log));
  try { await send('Browser.close'); } catch { /* it is killed below anyway */ }

  if (title === 'PASS') {
    console.log('check-page: PASS');
    finish(0);
  }
  if (title.startsWith('FAIL')) {
    console.log(`check-page: ${title}`);
    finish(1);
  }
  refuseRunning(`no verdict within ${timeoutS} s (the title is still '${title}')`);
}

function refuseRunning(why) {
  console.error(`check-page: ${why}`);
  finish(2);
}

main().catch((e) => refuseRunning(e.message));
