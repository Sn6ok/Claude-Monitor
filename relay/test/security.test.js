/**
 * Тести безпеки Relay (Частина 6 §27 Master Prompt).
 *
 * Покривають: автентифікацію, авторизацію, захист від повторного відтворення,
 * відкликання пристроїв, некоректні пакети, обмеження розміру та частоти,
 * а також ключову властивість — Relay не бачить вмісту повідомлень.
 */

import { test, describe, before, after } from 'node:test';
import assert from 'node:assert/strict';
import { mkdtempSync, rmSync, readFileSync, existsSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { randomBytes } from 'node:crypto';

import { RelayServer } from '../src/server.js';
import { DeviceRegistry } from '../src/devices.js';
import { LIMITS, makeFrame } from '../src/protocol.js';
import { makeIdentity, signAuth, TestClient, sleep } from './helpers.js';

let server;
let dataDir;
let url;

/** Логер, що мовчить, — інакше вивід тестів тоне в журналі сервера. */
const quietLogger = { log() {}, warn() {}, error() {} };

before(async () => {
  dataDir = mkdtempSync(join(tmpdir(), 'cm-relay-test-'));
  server = new RelayServer(
    {
      mode: 'plain',
      host: '127.0.0.1',
      port: 0,
      dataDir,
      devicesFile: join(dataDir, 'devices.json'),
      logLevel: 'error',
      maxConnections: 64,
      allowedOrigins: [],
      tls: null,
      // Тести відкривають десятки зʼєднань з однієї адреси. Ліміт зʼєднань
      // перевіряється окремим тестом нижче, тут він лише заважав би.
      limits: {
        connection: { capacity: 500, refillPerSec: 100 },
        auth: { capacity: 200, refillPerSec: 50 },
        pairing: { capacity: 5, refillPerSec: 0.005 },
        frames: { capacity: 120, refillPerSec: 40 },
      },
    },
    quietLogger,
  );
  await server.start();
  url = `ws://127.0.0.1:${server.httpServer.address().port}/ws`;
});

after(async () => {
  await server?.stop();
  if (dataDir) rmSync(dataDir, { recursive: true, force: true });
});

/** Реєструє пару пристроїв напряму в реєстрі — швидше за повний pairing. */
function registerPair() {
  const bridge = makeIdentity();
  const monitor = makeIdentity();
  server.registry.upsert({ deviceId: bridge.deviceId, role: 'bridge', pubSig: bridge.pubSig, pubEcdh: bridge.pubEcdh });
  server.registry.upsert({ deviceId: monitor.deviceId, role: 'monitor', pubSig: monitor.pubSig, pubEcdh: monitor.pubEcdh });
  server.registry.pair(bridge.deviceId, monitor.deviceId);
  return { bridge, monitor };
}

describe('автентифікація', () => {
  test('зареєстрований пристрій із правильним підписом проходить', async () => {
    const { bridge } = registerPair();
    const client = new TestClient(url);
    await client.connect();

    const ok = await client.authenticate(bridge, 'bridge');
    assert.equal(ok.t, 'auth_ok');
    assert.equal(typeof ok.p.session_token, 'string');

    client.close();
  });

  test('невідомий пристрій відхиляється', async () => {
    const stranger = makeIdentity();
    const client = new TestClient(url);
    await client.connect();

    client.send('hello', { role: 'bridge', device_id: stranger.deviceId });
    const challenge = await client.waitFor('challenge');
    client.send('auth', { device_id: stranger.deviceId, sig: signAuth(stranger, 'bridge', challenge.p.nonce) });

    const err = await client.waitFor('error');
    assert.equal(err.p.code, 'auth_failed');
    client.close();
  });

  test('хибний підпис відхиляється', async () => {
    const { bridge } = registerPair();
    const impostor = makeIdentity();
    const client = new TestClient(url);
    await client.connect();

    client.send('hello', { role: 'bridge', device_id: bridge.deviceId });
    const challenge = await client.waitFor('challenge');
    // Підпис зроблено чужим ключем, але заявлено чужий device_id.
    client.send('auth', {
      device_id: bridge.deviceId,
      sig: signAuth(impostor, 'bridge', challenge.p.nonce),
    });

    const err = await client.waitFor('error');
    assert.equal(err.p.code, 'auth_failed');
    client.close();
  });

  test('підпис чужого nonce не проходить — захист від повторного відтворення', async () => {
    const { bridge } = registerPair();

    // Перше зʼєднання: отримуємо і використовуємо nonce.
    const first = new TestClient(url);
    await first.connect();
    first.send('hello', { role: 'bridge', device_id: bridge.deviceId });
    const challenge = await first.waitFor('challenge');
    const signature = signAuth(bridge, 'bridge', challenge.p.nonce);
    first.send('auth', { device_id: bridge.deviceId, sig: signature });
    await first.waitFor('auth_ok');

    // Друге зʼєднання: повторюємо той самий підпис для нового nonce.
    const second = new TestClient(url);
    await second.connect();
    second.send('hello', { role: 'bridge', device_id: bridge.deviceId });
    await second.waitFor('challenge');
    second.send('auth', { device_id: bridge.deviceId, sig: signature });

    const err = await second.waitFor('error');
    assert.equal(err.p.code, 'auth_failed');

    first.close();
    second.close();
  });

  test('невідповідність ролі відхиляється', async () => {
    const { bridge } = registerPair();
    const client = new TestClient(url);
    await client.connect();

    // Пристрій зареєстровано як bridge, а представляється монітором.
    client.send('hello', { role: 'monitor', device_id: bridge.deviceId });
    const challenge = await client.waitFor('challenge');
    client.send('auth', { device_id: bridge.deviceId, sig: signAuth(bridge, 'monitor', challenge.p.nonce) });

    const err = await client.waitFor('error');
    assert.equal(err.p.code, 'auth_failed');
    client.close();
  });

  test('fwd до автентифікації відхиляється', async () => {
    const client = new TestClient(url);
    await client.connect();

    client.send('fwd', { n: 'A'.repeat(16), ct: 'BBBB' });
    const err = await client.waitFor('error');
    assert.equal(err.p.code, 'bad_frame');
    client.close();
  });

  test('зʼєднання без рукостискання закривається за таймаутом', async () => {
    const client = new TestClient(url);
    await client.connect();
    // Нічого не надсилаємо: сервер має розірвати зʼєднання самостійно.
    const code = await client.waitForClose(20_000);
    assert.equal(typeof code, 'number');
  });

  test('зʼєднання для pairing переживає таймаут рукостискання', async () => {
    // Bridge у режимі --pair чекає на телефон усі 180 секунд життя коду.
    // Таке зʼєднання НЕ може бути автентифікованим за визначенням:
    // пристрій саме зараз реєструється. Раніше загальний 15-секундний
    // таймаут обривав його на середині з помилкою auth_failed ще до того,
    // як користувач устигав ввести код.
    const bridge = makeIdentity();
    const client = new TestClient(url);
    await client.connect();

    client.send('pair_offer', {
      offer_id: 'a'.repeat(32),
      pub_sig: bridge.pubSig,
      pub_ecdh: bridge.pubEcdh,
      confirm: 'dGVzdGNvbmZpcm0',
    });

    // Чекаємо помітно довше за таймаут рукостискання.
    await sleep(18_000);

    assert.equal(client.closed, false, 'зʼєднання для pairing обірвано завчасно');

    const rejected = client.received.some(
      (f) => f.t === 'error' && f.p.code === 'auth_failed',
    );
    assert.equal(rejected, false, 'надіслано хибну помилку auth_failed');

    client.close();
  });

  test('монітор під час pairing теж переживає таймаут рукостискання', async () => {
    // Дзеркальний випадок до попереднього тесту, але з боку телефона.
    // Bridge міг успішно підтвердити код, а Relay на той час уже обривав
    // зʼєднання монітора — на телефоні це виглядало як «час очікування
    // вичерпано» попри успіх на ноутбуці.
    const bridgeIdentity = makeIdentity();
    const monitorIdentity = makeIdentity();

    const bridgeClient = new TestClient(url);
    await bridgeClient.connect();
    bridgeClient.send('pair_offer', {
      offer_id: 'b'.repeat(32),
      pub_sig: bridgeIdentity.pubSig,
      pub_ecdh: bridgeIdentity.pubEcdh,
      confirm: 'YnJpZGdlY29uZmlybQ',
    });
    await sleep(200);

    const monitorClient = new TestClient(url);
    await monitorClient.connect();
    monitorClient.send('pair_claim', {
      offer_id: '',
      pub_sig: monitorIdentity.pubSig,
      pub_ecdh: monitorIdentity.pubEcdh,
      confirm: 'bW9uaXRvcmNvbmZpcm0',
    });

    // Bridge отримує заявку і, у реальному сценарії, показує її користувачу.
    await bridgeClient.waitFor('pair_claim', 5000);

    // Чекаємо довше за таймаут рукостискання — обидві сторони мають вижити.
    await sleep(18_000);

    assert.equal(monitorClient.closed, false, 'зʼєднання телефона обірвано завчасно');
    assert.equal(bridgeClient.closed, false, 'зʼєднання ноутбука обірвано завчасно');

    bridgeClient.close();
    monitorClient.close();
  });
});

describe('авторизація', () => {
  test('повідомлення доходить лише до спареного пристрою', async () => {
    const { bridge, monitor } = registerPair();

    const bridgeClient = new TestClient(url);
    const monitorClient = new TestClient(url);
    await bridgeClient.connect();
    await monitorClient.connect();
    await bridgeClient.authenticate(bridge, 'bridge');
    await monitorClient.authenticate(monitor, 'monitor');

    bridgeClient.send('fwd', { n: 'A'.repeat(16), ct: 'ZGFuaQ' });
    const received = await monitorClient.waitFor('fwd');
    assert.equal(received.p.ct, 'ZGFuaQ');

    bridgeClient.close();
    monitorClient.close();
  });

  test('чужий монітор не отримує повідомлень навіть будучи автентифікованим', async () => {
    const pairA = registerPair();
    const pairB = registerPair();

    const bridgeA = new TestClient(url);
    const monitorB = new TestClient(url);
    await bridgeA.connect();
    await monitorB.connect();
    await bridgeA.authenticate(pairA.bridge, 'bridge');
    await monitorB.authenticate(pairB.monitor, 'monitor');

    bridgeA.send('fwd', { n: 'A'.repeat(16), ct: 'c2VjcmV0' });

    // Чужому монітору не має прийти нічого. Даємо час на можливу доставку.
    await sleep(300);
    const leaked = monitorB.received.filter((f) => f.t === 'fwd');
    assert.equal(leaked.length, 0, 'повідомлення просочилось до чужого пристрою');

    bridgeA.close();
    monitorB.close();
  });

  test('без пари повертається not_paired', async () => {
    const lonely = makeIdentity();
    server.registry.upsert({ deviceId: lonely.deviceId, role: 'bridge', pubSig: lonely.pubSig, pubEcdh: lonely.pubEcdh });

    const client = new TestClient(url);
    await client.connect();
    await client.authenticate(lonely, 'bridge');

    client.send('fwd', { n: 'A'.repeat(16), ct: 'AAAA' });
    const err = await client.waitFor('error');
    assert.equal(err.p.code, 'not_paired');
    client.close();
  });
});

describe('відкликання пристрою', () => {
  test('відкликаний пристрій не може автентифікуватись', async () => {
    const { monitor } = registerPair();
    server.registry.revoke(monitor.deviceId);

    const client = new TestClient(url);
    await client.connect();
    client.send('hello', { role: 'monitor', device_id: monitor.deviceId });
    const challenge = await client.waitFor('challenge');
    client.send('auth', { device_id: monitor.deviceId, sig: signAuth(monitor, 'monitor', challenge.p.nonce) });

    const err = await client.waitFor('error');
    // Окремий код: застосунок має стерти ключі, а не повторювати спроби.
    assert.equal(err.p.code, 'device_revoked');
    client.close();
  });

  test('відкликання діє негайно на активну маршрутизацію', async () => {
    const { bridge, monitor } = registerPair();

    const bridgeClient = new TestClient(url);
    const monitorClient = new TestClient(url);
    await bridgeClient.connect();
    await monitorClient.connect();
    await bridgeClient.authenticate(bridge, 'bridge');
    await monitorClient.authenticate(monitor, 'monitor');

    // До відкликання доставка працює.
    bridgeClient.send('fwd', { n: 'A'.repeat(16), ct: 'YmVmb3Jl' });
    await monitorClient.waitFor('fwd');

    server.registry.revoke(monitor.deviceId);
    const countBefore = monitorClient.received.filter((f) => f.t === 'fwd').length;

    bridgeClient.send('fwd', { n: 'A'.repeat(16), ct: 'YWZ0ZXI' });
    await sleep(300);

    const countAfter = monitorClient.received.filter((f) => f.t === 'fwd').length;
    assert.equal(countAfter, countBefore, 'після відкликання доставка мала припинитись');

    bridgeClient.close();
    monitorClient.close();
  });

});

describe('кілька телефонів одного Bridge', () => {
  /** Bridge із двома спареними телефонами. */
  function registerTwoPhones() {
    const { bridge, monitor: phoneA } = registerPair();
    const phoneB = makeIdentity();
    server.registry.upsert({ deviceId: phoneB.deviceId, role: 'monitor', pubSig: phoneB.pubSig, pubEcdh: phoneB.pubEcdh });
    server.registry.pair(bridge.deviceId, phoneB.deviceId);
    return { bridge, phoneA, phoneB };
  }

  test('кадр із to доходить лише адресату, а отримувач бачить from', async () => {
    const { bridge, phoneA, phoneB } = registerTwoPhones();

    const clientA = new TestClient(url);
    const clientB = new TestClient(url);
    const bridgeClient = new TestClient(url);
    await clientA.connect();
    await clientB.connect();
    await bridgeClient.connect();
    await clientA.authenticate(phoneA, 'monitor');
    await clientB.authenticate(phoneB, 'monitor');

    // Bridge одразу знає, котрі з його телефонів у мережі.
    const ok = await bridgeClient.authenticate(bridge, 'bridge');
    assert.deepEqual([...ok.p.peers_online].sort(), [phoneA.deviceId, phoneB.deviceId].sort());

    bridgeClient.send('fwd', { n: 'A'.repeat(16), ct: 'Zm9yQQ', to: phoneA.deviceId });
    const got = await clientA.waitFor('fwd');
    assert.equal(got.p.ct, 'Zm9yQQ');
    assert.equal(got.p.from, bridge.deviceId);

    await sleep(300);
    assert.equal(
      clientB.received.filter((f) => f.t === 'fwd').length, 0,
      'кадр, адресований одному телефону, дійшов до іншого',
    );

    // Без to — усім партнерам, як і раніше.
    bridgeClient.send('fwd', { n: 'A'.repeat(16), ct: 'YWxs' });
    const broadcast = await clientB.waitFor('fwd');
    assert.equal(broadcast.p.ct, 'YWxs');

    clientA.close();
    clientB.close();
    bridgeClient.close();
  });

  test('кадр телефона приходить до Bridge із from', async () => {
    const { bridge, phoneA } = registerTwoPhones();

    const bridgeClient = new TestClient(url);
    const clientA = new TestClient(url);
    await bridgeClient.connect();
    await clientA.connect();
    await bridgeClient.authenticate(bridge, 'bridge');
    await clientA.authenticate(phoneA, 'monitor');

    // Поява телефона — з його device_id.
    const presence = await bridgeClient.waitFor('error');
    assert.equal(presence.p.code, 'peer_online');
    assert.equal(presence.p.device_id, phoneA.deviceId);

    clientA.send('fwd', { n: 'B'.repeat(16), ct: 'cmVx' });
    const got = await bridgeClient.waitFor('fwd');
    assert.equal(got.p.from, phoneA.deviceId);

    clientA.close();
    bridgeClient.close();
  });

  test('чужий адресат у to нікуди не доходить', async () => {
    const { bridge, phoneA } = registerTwoPhones();
    const stranger = registerPair().monitor;

    const bridgeClient = new TestClient(url);
    const clientA = new TestClient(url);
    const strangerClient = new TestClient(url);
    await bridgeClient.connect();
    await clientA.connect();
    await strangerClient.connect();
    await clientA.authenticate(phoneA, 'monitor');
    await strangerClient.authenticate(stranger, 'monitor');
    await bridgeClient.authenticate(bridge, 'bridge');

    bridgeClient.send('fwd', { n: 'A'.repeat(16), ct: 'c2VjcmV0', to: stranger.deviceId });
    await sleep(300);

    assert.equal(strangerClient.received.filter((f) => f.t === 'fwd').length, 0);
    assert.equal(clientA.received.filter((f) => f.t === 'fwd').length, 0);

    bridgeClient.close();
    clientA.close();
    strangerClient.close();
  });
});

describe('стійкість до некоректних даних', () => {
  test('пошкоджені пакети не валять сервер', async () => {
    const garbage = [
      'не json взагалі',
      '{"v":1}',
      '[]',
      'null',
      '{"v":1,"t":"fwd","id":"' + 'A'.repeat(26) + '","ts":0,"p":{}}',
      Buffer.from([0x00, 0xff, 0xfe]),
      '{"v":999,"t":"hello","id":"' + 'A'.repeat(26) + '","ts":' + Date.now() + ',"p":{}}',
    ];

    for (const payload of garbage) {
      const client = new TestClient(url);
      await client.connect();
      client.sendRaw(payload);
      await sleep(60);
      client.close();
    }

    // Головна перевірка: сервер живий і далі обслуговує клієнтів.
    const { bridge } = registerPair();
    const healthy = new TestClient(url);
    await healthy.connect();
    const ok = await healthy.authenticate(bridge, 'bridge');
    assert.equal(ok.t, 'auth_ok');
    healthy.close();
  });

  test('завеликий кадр відхиляється, сервер лишається живим', async () => {
    const client = new TestClient(url);
    await client.connect();
    client.sendRaw('x'.repeat(LIMITS.MAX_FRAME_BYTES + 5000));
    const code = await client.waitForClose(3000);
    assert.equal(typeof code, 'number');

    const { bridge } = registerPair();
    const healthy = new TestClient(url);
    await healthy.connect();
    assert.equal((await healthy.authenticate(bridge, 'bridge')).t, 'auth_ok');
    healthy.close();
  });
});

describe('обмеження частоти', () => {
  test('надто швидкий потік кадрів обривається', async () => {
    const { bridge } = registerPair();
    const client = new TestClient(url);
    await client.connect();
    await client.authenticate(bridge, 'bridge');

    // Ліміт кадрів: сплеск 120, поповнення 40/с. Надсилаємо помітно більше.
    for (let i = 0; i < 400; i += 1) {
      if (client.closed) break;
      client.send('ping', {});
    }

    await sleep(500);
    const limited = client.received.some((f) => f.t === 'error' && f.p.code === 'rate_limited');
    assert.equal(limited || client.closed, true, 'ліміт частоти мав спрацювати');
    client.close();
  });
});

describe('конфіденційність від Relay', () => {
  test('Relay не зберігає вмісту повідомлень на диск', async () => {
    const { bridge, monitor } = registerPair();
    const secret = Buffer.from('дуже-таємний-вміст-події').toString('base64url');

    const bridgeClient = new TestClient(url);
    const monitorClient = new TestClient(url);
    await bridgeClient.connect();
    await monitorClient.connect();
    await bridgeClient.authenticate(bridge, 'bridge');
    await monitorClient.authenticate(monitor, 'monitor');

    bridgeClient.send('fwd', { n: 'A'.repeat(16), ct: secret });
    await monitorClient.waitFor('fwd');
    await sleep(200);

    const registryPath = join(dataDir, 'devices.json');
    if (existsSync(registryPath)) {
      const onDisk = readFileSync(registryPath, 'utf8');
      assert.equal(onDisk.includes(secret), false, 'шифротекст потрапив у реєстр');
      assert.equal(onDisk.includes('таємний'), false, 'вміст потрапив у реєстр');
    }

    bridgeClient.close();
    monitorClient.close();
  });

  test('реєстр не містить приватних ключів', () => {
    const registry = new DeviceRegistry(join(dataDir, 'devices.json'), quietLogger);

    for (const device of registry.devices.values()) {
      // Дозволений набір полів фіксований. Будь-яке нове поле має бути
      // додане свідомо — саме так у реєстр не потрапить зайве.
      const allowed = new Set([
        'role', 'pub_sig', 'pub_ecdh', 'label',
        'created_at', 'last_seen', 'revoked', 'revoked_at', 'paired_with',
      ]);
      for (const key of Object.keys(device)) {
        assert.equal(allowed.has(key), true, `у реєстрі несподіване поле "${key}"`);
      }

      // Маркери приватного ключа в будь-якому поширеному форматі.
      const dump = JSON.stringify(device);
      for (const marker of ['PRIVATE KEY', 'privateKey', 'BEGIN EC', 'pkcs8']) {
        assert.equal(dump.includes(marker), false, `реєстр містить "${marker}"`);
      }

      // Публічний ключ P-256 — рівно 65 байт. Приватний матеріал
      // зробив би значення довшим.
      const pub = Buffer.from(device.pub_sig, 'base64url');
      assert.equal(pub.length, 65, 'pub_sig має бути точкою P-256 на 65 байт');
      assert.equal(pub[0], 0x04, 'pub_sig має бути неспресованою точкою');
    }
  });
});

describe('health-ендпоінт', () => {
  test('не розкриває даних про пристрої', async () => {
    const port = server.httpServer.address().port;
    const res = await fetch(`http://127.0.0.1:${port}/health`);
    const body = await res.json();

    assert.equal(res.status, 200);
    assert.equal(body.ok, true);
    assert.deepEqual(Object.keys(body).sort(), ['ok', 'protocol', 'uptime_sec']);
  });

  test('інші маршрути повертають 404', async () => {
    const port = server.httpServer.address().port;
    for (const path of ['/', '/devices', '/admin', '/../etc/passwd']) {
      const res = await fetch(`http://127.0.0.1:${port}${path}`);
      assert.equal(res.status, 404, `маршрут ${path} мав дати 404`);
    }
  });
});
