#!/usr/bin/env node
// tools/web/check-page.mjs <dir>: runs FunkGui::web's browser check (test/web/page.cpp) in headless Chrome and reports
// its verdict (v0.12.0; FCompressor ADR-93, docs/sprints/web-b.md G-C). <dir> is the page as the `web` preset built it:
// <build-web>/test/web, holding index.html, funkgui_web_page.js and funkgui_web_page.wasm. CTest runs it as
// fg.web.page (labels fg;live: never in `verify`, which has no browser):
//
//   cmake --workflow --preset web-verify && tools/web/check-page.mjs build-web/test/web
//   ctest --test-dir build-web -L live --output-on-failure
//
// Another page of that directory (v0.13.0): --page <stem> opens <stem>.html, whose module is funkgui_web_<stem>.js and
// .wasm (or <stem>.js and .wasm), and is otherwise the same run with the same verdict protocol: the title RUNNING, then
// PASS or "FAIL: <why>", and the log in the element funkgui-log. CTest runs WebHost's page (test/web/host.cpp) as
// fg.web.host:
//
//   tools/web/check-page.mjs build-web/test/web --page host
//
// What it does: serves <dir> with `python3 -m http.server` on 127.0.0.1 (a free port), starts Chrome headless with its
// own throwaway profile, opens index.html through the DevTools protocol, waits until document.title is "PASS" or
// starts with "FAIL", prints the page's log (its <pre>), and stops Chrome and the server whatever happened.
// Exit code: 0 the page said PASS; 1 it said FAIL; 2 the check could not run (no Chrome, no python3, no page, the
// browser went away, no verdict within the timeout). Nothing else exits 0: the code is 2 until a PASS was read.
// Nothing is installed or downloaded; node (which Emscripten needs anyway) is the only interpreter besides python3.
//
//   --page <stem>         the page to open: <stem>.html (default: index.html, the sink's page)
//   --chrome <path>       the browser (default: $CHROME, else Google Chrome's usual place on macOS, else
//                         google-chrome, chromium or chromium-browser on PATH)
//   --chrome-flag <flag>  an extra Chrome flag (repeatable). Without any, the GPU path is ANGLE on Metal on macOS and
//                         SwiftShader elsewhere (--use-angle=swiftshader --enable-unsafe-swiftshader): WebGL2 either
//                         way. Giving one replaces that default.
//   --timeout <seconds>   how long to wait for the verdict (default 90). It bounds the whole run, whatever the page
//                         or the browser does: every DevTools request ends at it (a page whose main thread spins
//                         answers none), and a browser that exits or drops its socket ends the run at once. The
//                         log printed then is the page's as last read.
//
// By hand instead: python3 -m http.server 8137 --bind 127.0.0.1 --directory <dir>, then open
// http://127.0.0.1:8137/index.html in any browser and read the tab's title.

import { spawn, spawnSync } from 'node:child_process';
import { existsSync, mkdtempSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join, resolve } from 'node:path';

const usage = 'usage: tools/web/check-page.mjs <dir with index.html> [--page <stem>] [--chrome <path>] '
            + '[--chrome-flag <flag>]... [--timeout <seconds>]';

function refuse(why) {
  console.error(`check-page: ${why}`);
  process.exit(2);
}

// ---- arguments ------------------------------------------------------------------------------------------------------
let dir = '', chrome = process.env.CHROME || '', timeoutS = 90, page = '';
const flags = [];
for (let i = 2; i < process.argv.length; ++i) {
  const a = process.argv[i];
  if (a === '--chrome') chrome = process.argv[++i] || '';
  else if (a === '--page') page = process.argv[++i] || '';
  else if (a === '--chrome-flag') flags.push(process.argv[++i] || '');
  else if (a === '--timeout') timeoutS = Number(process.argv[++i]);
  else if (a.startsWith('-') || dir) refuse(usage);
  else dir = a;
}
if (!dir || !(timeoutS > 0)) refuse(usage);
if (process.argv.includes('--page') && !/^[A-Za-z0-9][A-Za-z0-9._-]*$/.test(page)) refuse(usage);
dir = resolve(dir);
// The page and its module. Without --page: the sink's page, as before. With it: <stem>.html and the module the build
// names funkgui_web_<stem> (or <stem> itself).
const html = page ? `${page}.html` : 'index.html';
const modules = page ? [`funkgui_web_${page}`, page] : ['funkgui_web_page'];
const module = modules.find((m) => existsSync(join(dir, `${m}.js`))) || modules[0];
for (const f of [html, `${module}.js`, `${module}.wasm`])
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
process.exitCode = 2;          // whatever ends this process without finish(): never 0 without a PASS
const children = [];
let profile = '';
let finishing = false;         // a verdict was read, or the run is ending: the browser going away is expected
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
  finishing = true;
  cleanUp();
  process.exit(code);
}
function refuseRunning(why) {
  console.error(`check-page: ${why}`);
  finish(2);
}
process.on('SIGINT', () => finish(2));
process.on('SIGTERM', () => finish(2));
process.on('uncaughtException', (e) => refuseRunning(`${(e && e.message) || e}`));   // e.g. a reply that is not JSON

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const deadline = Date.now() + timeoutS * 1000;
const untilDeadline = () => Math.max(0, deadline - Date.now());
const closeMs = 1000;          // how long the browser has to answer Browser.close, after the verdict

// A DevTools request that got no reply within its bound.
class Unanswered extends Error {}

// The page's log as last read, printed once, on whichever path ends the run.
let log = '';
function printLog() {
  process.stdout.write(log);
  log = '';
}

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
    setTimeout(() => done(null), untilDeadline());
  });
}

async function main() {
  // python prints "Serving HTTP on 127.0.0.1 port N" once it listens; port 0 asks the system for a free one.
  const server = spawn('python3', ['-u', '-m', 'http.server', '0', '--bind', '127.0.0.1', '--directory', dir],
                       { stdio: ['ignore', 'pipe', 'pipe'] });
  children.push(server);
  const serving = await lineFrom(server, /Serving HTTP on 127\.0\.0\.1 port (\d+)/);
  if (!serving) refuseRunning('python3 -m http.server did not start');
  const url = `http://127.0.0.1:${serving[1]}/${html}`;

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
    setTimeout(() => bad(new Error(`the DevTools socket did not open within ${timeoutS} s`)), untilDeadline());
  });
  // From here no reply can come once the browser is gone, so its end is the run's (unless this script closed it).
  const gone = (how) => {
    if (finishing) return;
    printLog();
    refuseRunning(`the browser went away before a verdict (${how})`);
  };
  browser.on('exit', (code, signal) => gone(`its process exited: ${signal || `code ${code}`}`));
  socket.addEventListener('close', () => gone('the DevTools socket closed'));
  socket.addEventListener('error', () => gone('the DevTools socket failed'));

  let nextId = 0;
  const waiting = new Map();
  socket.addEventListener('message', (event) => {
    const message = JSON.parse(event.data);
    if (message.id !== undefined && waiting.has(message.id)) {
      const { answer, reject, timer } = waiting.get(message.id);
      waiting.delete(message.id);
      clearTimeout(timer);
      if (message.error) reject(new Error(message.error.message));
      else answer(message.result);
    }
  });
  // One request, bounded: by the deadline unless a bound is given. A reply may never come (a page whose main thread
  // spins answers no Runtime.evaluate), and the deadline is otherwise only looked at between replies.
  const send = (method, params = {}, sessionId = undefined, boundMs = untilDeadline()) =>
    new Promise((answer, reject) => {
      const id = ++nextId;
      const timer = setTimeout(() => {
        waiting.delete(id);
        reject(new Unanswered(`${method} was not answered`));
      }, boundMs);
      waiting.set(id, { answer, reject, timer });
      socket.send(JSON.stringify({ id, method, params, sessionId }));
    });
  const { targetId } = await send('Target.createTarget', { url });
  const { sessionId } = await send('Target.attachToTarget', { targetId, flatten: true });
  const evaluate = async (expression) => {
    const { result } = await send('Runtime.evaluate', { expression, returnByValue: true }, sessionId);
    return result.value;
  };

  console.log(`check-page: ${url} in ${chrome} (${flags.join(' ')})`);
  // The title and the log in one read, so the log is in hand whenever the run ends: at the verdict it is complete
  // (the page writes its last line before the title), and at a timeout it is what the page had said when it last
  // answered.
  const state = "[document.title, (document.getElementById('funkgui-log') || {}).textContent || '']";
  const isVerdict = (t) => t === 'PASS' || t.startsWith('FAIL');
  let title = '', answers = true;
  try {
    while (Date.now() < deadline) {
      const read = await evaluate(state);
      if (Array.isArray(read)) [title, log] = read.map(String);
      if (isVerdict(title)) break;
      await sleep(Math.min(100, untilDeadline()));
    }
  } catch (e) {
    if (!(e instanceof Unanswered)) throw e;
    answers = false;           // the deadline ended a read the page never answered
  }
  printLog();
  if (!isVerdict(title))
    refuseRunning(answers ? `no verdict within ${timeoutS} s (the title is still '${title}')`
                          : `no verdict within ${timeoutS} s (the page stopped answering; its title was '${title}')`);

  finishing = true;            // the verdict is in: closing the browser is this script's doing, not a failure
  try { await send('Browser.close', {}, undefined, closeMs); } catch { /* it is killed below anyway */ }
  if (title === 'PASS') {
    console.log('check-page: PASS');
    finish(0);
  }
  console.log(`check-page: ${title}`);
  finish(1);
}

main().catch((e) => refuseRunning(e instanceof Unanswered ? `no verdict within ${timeoutS} s (${e.message})`
                                                          : e.message));
