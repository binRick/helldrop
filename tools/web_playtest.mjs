// Browser regression runner. Uses Chrome DevTools directly, with real pointer input.
import assert from 'node:assert/strict';
import { spawn } from 'node:child_process';
import { once } from 'node:events';
import { createReadStream } from 'node:fs';
import { mkdtemp, readFile, rm } from 'node:fs/promises';
import { createServer } from 'node:http';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { setTimeout as delay } from 'node:timers/promises';

const types = { '/index.html': 'text/html', '/index.js': 'text/javascript',
  '/index.wasm': 'application/wasm', '/index.data': 'application/octet-stream',
  '/models.pack': 'application/octet-stream' };

async function until(check, message, timeout = 15000) {
  const end = Date.now() + timeout;
  while (Date.now() < end) {
    const value = await check();
    if (value) return value;
    await delay(50);
  }
  throw new Error(message);
}

const profile = await mkdtemp(join(tmpdir(), 'helldrop-test-'));
const server = createServer((request, response) => {
  const pathname = request.url.split('?')[0];
  const file = pathname === '/' ? '/index.html' : pathname;
  if (!Object.hasOwn(types, file)) {
    response.writeHead(404).end();
    return;
  }
  const stream = createReadStream(new URL('../dist-web' + file, import.meta.url));
  stream.on('open', () => {
    response.setHeader('Content-Type', types[file]);
    stream.pipe(response);
  });
  stream.on('error', () => response.writeHead(404).end());
});
server.listen(0, '127.0.0.1');
await once(server, 'listening');

const chrome = process.env.HELLDROP_CHROME || (process.platform === 'darwin'
  ? '/Applications/Google Chrome.app/Contents/MacOS/Google Chrome' : 'chromium');
const browser = spawn(chrome, [
  '--headless', '--no-first-run', '--remote-debugging-port=0',
  '--enable-unsafe-swiftshader', '--use-angle=swiftshader', '--user-data-dir=' + profile,
  'about:blank'
], { stdio: 'ignore' });
let launchError;
browser.on('error', error => { launchError = error; });

let socket;
try {
  const portFile = await until(async () => {
    if (launchError) throw launchError;
    return readFile(join(profile, 'DevToolsActivePort'), 'utf8').catch(() => null);
  }, 'Chrome failed to launch');
  const port = portFile.split('\n')[0];
  const pages = await (await fetch('http://127.0.0.1:' + port + '/json/list')).json();
  socket = new WebSocket(pages.find(page => page.url === 'about:blank').webSocketDebuggerUrl);
  await once(socket, 'open');

  let sequence = 0;
  const pending = new Map();
  const errors = [];
  const requests = [];
  socket.addEventListener('message', event => {
    const message = JSON.parse(event.data);
    if (message.id) {
      const callbacks = pending.get(message.id);
      pending.delete(message.id);
      if (message.error) callbacks.reject(new Error(message.error.message));
      else callbacks.resolve(message.result);
    } else if (message.method === 'Runtime.exceptionThrown') {
      errors.push(message.params.exceptionDetails);
    } else if (message.method === 'Network.requestWillBeSent') {
      requests.push(message.params.request.url);
    }
  });
  function call(method, params = {}) {
    return new Promise((resolve, reject) => {
      const id = ++sequence;
      pending.set(id, { resolve, reject });
      socket.send(JSON.stringify({ id, method, params }));
    });
  }

  async function evaluate(expression) {
    const result = await call('Runtime.evaluate', { expression, returnByValue: true });
    if (result.exceptionDetails) throw new Error(JSON.stringify(result.exceptionDetails));
    return result.result.value;
  }
  async function wait(expression) {
    try {
      return await until(() => evaluate(expression), 'Timed out waiting for ' + expression);
    } catch (error) {
      const state = await evaluate("typeof currentLevel === 'undefined' ? null : {currentLevel, step, stats: stats.textContent, notice: notice.textContent, mute: mute.textContent, savedMute: localStorage.getItem('helldrop.muted')}");
      throw new Error(error.message + ' ' + JSON.stringify(state) + ' ' + JSON.stringify(errors));
    }
  }
  const elementCenter = selector => evaluate(`(() => {
    const rect = document.querySelector(${JSON.stringify(selector)}).getBoundingClientRect();
    return [rect.left + rect.width / 2, rect.top + rect.height / 2];
  })()`);
  const canvasPoint = (x, y) => evaluate(`(() => {
    const rect = canvas.getBoundingClientRect();
    return [rect.left + rect.width * ${x}, rect.top + rect.height * ${y}];
  })()`);

  async function trainingSpot(east, x, y) {
    const aspect = await evaluate('canvas.width / canvas.height');
    const yaw = (aspect < 1 ? 72 : 45) * Math.PI / 180;
    const pitch = 62 * Math.PI / 180;
    const horizontal = east * Math.cos(yaw) + 224 * Math.sin(yaw);
    const vertical = (east * Math.sin(yaw) + 224 * Math.cos(yaw)) * Math.sin(pitch);
    const fovy = Math.max((horizontal + 44) / aspect, vertical + 104);
    const dx = x - east / 2;
    const dy = y - 112;
    return canvasPoint(0.5 + (dx * Math.cos(yaw) + dy * Math.sin(yaw)) / (fovy * aspect),
      0.5 + ((dx * Math.sin(yaw) - dy * Math.cos(yaw)) * Math.sin(pitch) +
        27 * Math.cos(pitch)) / fovy);
  }
  async function selectLevel(level) {
    await evaluate(`levelPicker.value = '${level}'; levelPicker.dispatchEvent(new Event('change'))`);
    await wait(`currentLevel === ${level}`);
  }

  async function touch(type, point) {
    const touchPoints = point ? [{ x: point[0], y: point[1] }] : [];
    await call('Input.dispatchTouchEvent', { type, touchPoints });
  }
  async function tap(point) {
    await touch('touchStart', point);
    await touch('touchEnd');
  }
  const button = async selector => {
    const [x, y] = await elementCenter(selector);
    const hit = await evaluate(`document.elementFromPoint(${x}, ${y})?.closest('button')?.id`);
    assert.equal(hit, selector.slice(1), 'Button center should be visible and tappable');
    await tap([x, y]);
  };

  for (const domain of ['Runtime', 'Network', 'Page']) await call(domain + '.enable');
  await call('Page.addScriptToEvaluateOnNewDocument', { source: `
    window.testAudioPeak = 0;
    const connect = AudioNode.prototype.connect;
    AudioNode.prototype.connect = function(target, ...args) {
      const result = connect.call(this, target, ...args);
      if (target instanceof AudioDestinationNode && !window.testAudioProbe) {
        const analyser = this.context.createAnalyser();
        window.testAudioProbe = analyser;
        connect.call(this, analyser);
        const samples = new Float32Array(analyser.fftSize);
        setInterval(() => {
          analyser.getFloatTimeDomainData(samples);
          for (const sample of samples) window.testAudioPeak = Math.max(window.testAudioPeak, Math.abs(sample));
        }, 25);
      }
      return result;
    };
  ` });

  const url = 'http://127.0.0.1:' + server.address().port + '/';
  await call('Emulation.setDeviceMetricsOverride', { width: 390, height: 844, deviceScaleFactor: 1, mobile: true });
  await call('Emulation.setTouchEmulationEnabled', { enabled: true, maxTouchPoints: 1 });
  await call('Page.navigate', { url });
  await wait("document.querySelector('#start:enabled')");
  await button('#start');
  await wait("currentLevel === 11 && step === 0 && instruction.textContent.includes('green ring')");
  await tap(await trainingSpot(720, 460, 128));
  await wait('step === 3');
  await button('#continue');
  await wait('currentLevel === 12 && step === 0');
  await tap(await trainingSpot(480, 304, 96));
  await wait('step === 3');
  await button('#continue');
  await wait("currentLevel === 0 && instruction.textContent.includes('green ring')");
  console.log('PASS First Drop and In the Shadows in order');

  await button('#mute');
  await wait("mute.textContent === 'Unmute' && localStorage.getItem('helldrop.muted') === 'true'");
  await button('#mute');
  await wait("mute.textContent === 'Mute' && localStorage.getItem('helldrop.muted') === 'false'");
  await button('#pause');
  await wait("pauseButton.textContent === 'Resume'");
  await tap(await canvasPoint(0.62, 0.54));
  assert.equal(await evaluate('step'), 0, 'Paused game accepted a drop');
  await button('#pause');
  await wait("pauseButton.textContent === 'Pause'");

  await button('#shotgunner');
  await wait("shotgunner.getAttribute('aria-pressed') === 'true'");
  await button('#imp');
  await tap(await canvasPoint(0.62, 0.54));
  await wait("barrel.classList.contains('now')");
  await button('#blast');
  await wait('step === 3');
  await wait('testAudioPeak > 0.1');
  assert.ok(await evaluate("Number(localStorage.getItem('helldrop.bestBlast')) >= 65"));
  assert.ok(!requests.some(url => url.includes('models.pack')));
  console.log('PASS mobile ambush, pause, mute, demon selection, audio and saved score');

  await button('#continue');
  await wait('currentLevel === 1 && step === 0');
  await tap(await canvasPoint(0.62, 0.54));
  await wait("barrel.classList.contains('now')");
  await button('#blast');
  await wait('step === 3');
  assert.match(await evaluate('stats.textContent'), /2 barrels chained/);
  assert.ok(await evaluate("Number(localStorage.getItem('helldrop.bestChain')) >= 65"));
  await button('#retry');
  await wait("step === 0 && stats.textContent.includes('65%')");
  assert.ok(await evaluate("Number(localStorage.getItem('helldrop.bestChain')) >= 65"));
  console.log('PASS chain reaction and deterministic retry');

  await selectLevel(13);
  await tap(await trainingSpot(590, 305, 104));
  await wait('step === 12');
  await button('#door');
  await wait('step === 3');
  await button('#continue');
  await wait('currentLevel === 14 && step === 0');
  await tap(await trainingSpot(610, 306, 58));
  await wait('step === 13');
  await tap(await trainingSpot(610, 306, 133));
  await wait('step === 3');
  await button('#continue');
  await wait('currentLevel === 15 && step === 0');
  await tap(await trainingSpot(720, 372, 151));
  await wait('step === 13');
  await tap(await trainingSpot(720, 451, 143));
  await wait('step === 12');
  await button('#door');
  await wait("barrel.classList.contains('now')");
  await button('#blast');
  await wait('step === 3');
  assert.equal(await evaluate("JSON.parse(localStorage.getItem('helldrop.training')).length"), 7);
  console.log('PASS Hold the Door, Crossfire, The Gauntlet and saved training progress');
  await button('#continue');
  await wait('currentLevel === 2 && step === 5');
  await wait("Module.FS.analyzePath('/assets/models/marine.glb').exists");
  await button('#door');
  await wait("door.getAttribute('aria-pressed') === 'true' && instruction.textContent.includes('door or lift')");
  await button('#blast');
  await wait("barrel.getAttribute('aria-pressed') === 'true' && instruction.textContent.includes('near a barrel')");
  await button('#blast');
  await wait("barrel.getAttribute('aria-pressed') === 'false'");
  const souls = await evaluate("Number(stats.textContent.split(' ')[0])");
  const drag = await canvasPoint(0.70, 0.33);
  await touch('touchStart', drag);
  await touch('touchMove', [drag[0] - 55, drag[1] + 25]);
  await touch('touchEnd');
  await button('#zoom-in');
  await button('#zoom-out');
  await button('#follow');
  await button('#doomcam');
  await button('#doomcam');
  assert.ok(await evaluate("Number(stats.textContent.split(' ')[0])") >= souls,
    'Camera gesture spent souls');
  for (let level = 3; level <= 10; level++) {
    await evaluate(`levelPicker.value = '${level}'; levelPicker.dispatchEvent(new Event('change'))`);
    await wait(`currentLevel === ${level} && step === 5`);
  }
  console.log('PASS all nine Episode 1 maps, lazy models, touch tools and camera controls');

  await call('Emulation.setTouchEmulationEnabled', { enabled: false });
  await call('Emulation.setDeviceMetricsOverride', { width: 1280, height: 800, deviceScaleFactor: 1, mobile: false });
  async function click(point) {
    const [x, y] = point;
    await call('Input.dispatchMouseEvent', { type: 'mousePressed', button: 'left', clickCount: 1, x, y });
    await call('Input.dispatchMouseEvent', { type: 'mouseReleased', button: 'left', clickCount: 1, x, y });
  }
  await call('Page.reload');
  await wait("document.querySelector('#start:enabled')");
  await click(await elementCenter('#start'));
  await wait("currentLevel === 11 && step === 0 && instruction.textContent.includes('green ring')");
  await click(await trainingSpot(720, 460, 128));
  await wait('step === 10 || step === 6 || step === 3');
  await call('Input.dispatchKeyEvent', { type: 'keyDown', key: 'g', code: 'KeyG', windowsVirtualKeyCode: 71 });
  await call('Input.dispatchKeyEvent', { type: 'keyUp', key: 'g', code: 'KeyG', windowsVirtualKeyCode: 71 });
  await wait('step === 0');
  assert.deepEqual(errors, []);
  console.log('PASS desktop mouse placement and keyboard restart');
} finally {
  socket?.close();
  server.closeAllConnections();
  server.close();
  if (browser.pid && browser.exitCode === null) {
    browser.kill();
    await once(browser, 'exit');
  }
  await rm(profile, { recursive: true, force: true, maxRetries: 5, retryDelay: 100 });
}
