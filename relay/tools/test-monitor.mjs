#!/usr/bin/env node
/**
 * Тестовий монітор — імітує Android-застосунок.
 *
 * Призначення: наскрізна перевірка ланцюга Bridge -> Relay -> монітор
 * без потреби в телефоні. Реалізує ту саму криптографію й той самий
 * протокол, що й майбутній Kotlin-клієнт, тож слугує ще й референсом
 * для нього.
 *
 * Використання:
 *   node scripts/test-monitor.mjs pair <relay-url> <код>
 *   node scripts/test-monitor.mjs watch <relay-url>
 */

import { WebSocket } from 'ws';
import {
  generateKeyPairSync, createPublicKey, createPrivateKey, createSign,
  createHash, createHmac, createDecipheriv, createCipheriv,
  diffieHellman, hkdfSync, randomBytes,
} from 'node:crypto';
import { readFileSync, writeFileSync, existsSync, mkdirSync } from 'node:fs';
import { homedir } from 'node:os';
import { join } from 'node:path';

const STORE_DIR = join(homedir(), '.claude-monitor', 'test-monitor');
const STORE_FILE = join(STORE_DIR, 'identity.json');

// ── Криптографія (дзеркало Bridge) ───────────────────────────────────────────

function rawPointFromKey(publicKey) {
  const jwk = publicKey.export({ format: 'jwk' });
  return Buffer.concat([
    Buffer.from([0x04]),
    Buffer.from(jwk.x, 'base64url'),
    Buffer.from(jwk.y, 'base64url'),
  ]);
}

function keyFromRawPoint(raw) {
  if (raw.length !== 65 || raw[0] !== 0x04) throw new Error('некоректна точка P-256');
  return createPublicKey({
    key: {
      kty: 'EC', crv: 'P-256',
      x: raw.subarray(1, 33).toString('base64url'),
      y: raw.subarray(33, 65).toString('base64url'),
    },
    format: 'jwk',
  });
}

function makeIdentity() {
  const sign = generateKeyPairSync('ec', { namedCurve: 'prime256v1' });
  const ecdh = generateKeyPairSync('ec', { namedCurve: 'prime256v1' });

  const pubSign = rawPointFromKey(sign.publicKey);
  const pubEcdh = rawPointFromKey(ecdh.publicKey);

  return {
    signPrivPem: sign.privateKey.export({ type: 'pkcs8', format: 'pem' }),
    ecdhPrivPem: ecdh.privateKey.export({ type: 'pkcs8', format: 'pem' }),
    pubSig: pubSign.toString('base64url'),
    pubEcdh: pubEcdh.toString('base64url'),
    deviceId: createHash('sha256').update(pubSign).digest('hex'),
  };
}

function loadIdentity() {
  if (!existsSync(STORE_FILE)) return null;
  try {
    return JSON.parse(readFileSync(STORE_FILE, 'utf8'));
  } catch {
    return null;
  }
}

function saveIdentity(identity) {
  if (!existsSync(STORE_DIR)) mkdirSync(STORE_DIR, { recursive: true });
  writeFileSync(STORE_FILE, JSON.stringify(identity, null, 2), { mode: 0o600 });
}

/** Код підтвердження — має точно збігатися з ComputeConfirm у pairing.cpp. */
function computeConfirm(code, role, pubSig, pubEcdh) {
  const pairingKey = Buffer.from(
    hkdfSync('sha256', Buffer.from(code, 'utf8'),
             Buffer.from('claude-monitor-v1/pairing', 'utf8'),
             Buffer.alloc(0), 32));
  return createHmac('sha256', pairingKey)
    .update(role + pubSig + pubEcdh, 'utf8')
    .digest('base64url');
}

/** Спільний ключ E2EE — дзеркало Identity::DeriveSessionKey. */
function deriveSessionKey(identity, peerPubEcdh, myDeviceId, peerDeviceId) {
  const priv = createPrivateKey(identity.ecdhPrivPem);
  const peer = keyFromRawPoint(Buffer.from(peerPubEcdh, 'base64url'));
  const shared = diffieHellman({ privateKey: priv, publicKey: peer });

  // Сіль — обидва ідентифікатори у сталому порядку, щоб обидві сторони
  // отримали однаковий ключ незалежно від того, хто рахує.
  const salt = myDeviceId < peerDeviceId ? myDeviceId + peerDeviceId
                                         : peerDeviceId + myDeviceId;

  return Buffer.from(hkdfSync('sha256', shared, Buffer.from(salt, 'utf8'),
                              Buffer.from('claude-monitor-v1/e2ee', 'utf8'), 32));
}

function decryptPayload(sessionKey, nonceB64, ctB64) {
  const nonce = Buffer.from(nonceB64, 'base64url');
  const full = Buffer.from(ctB64, 'base64url');
  const ct = full.subarray(0, full.length - 16);
  const tag = full.subarray(full.length - 16);

  const decipher = createDecipheriv('aes-256-gcm', sessionKey, nonce);
  decipher.setAuthTag(tag);
  return Buffer.concat([decipher.update(ct), decipher.final()]).toString('utf8');
}

let monitorNonceCounter = 0n;
function encryptPayload(sessionKey, json) {
  // Напрямок "M2B " — монітор до Bridge. Різні напрямки не можуть
  // дати однаковий nonce.
  const nonce = Buffer.alloc(12);
  nonce.write('M2B ', 0, 'latin1');
  nonce.writeBigUInt64BE(monitorNonceCounter++, 4);

  const cipher = createCipheriv('aes-256-gcm', sessionKey, nonce);
  const ct = Buffer.concat([cipher.update(json, 'utf8'), cipher.final()]);
  return { n: nonce.toString('base64url'), ct: Buffer.concat([ct, cipher.getAuthTag()]).toString('base64url') };
}

// ── Протокол ─────────────────────────────────────────────────────────────────

const CROCKFORD = '0123456789ABCDEFGHJKMNPQRSTVWXYZ';
function makeUlid() {
  let time = Date.now();
  let out = '';
  for (let i = 0; i < 10; i += 1) { out = CROCKFORD[time % 32] + out; time = Math.floor(time / 32); }
  const rnd = randomBytes(16);
  for (let i = 0; i < 16; i += 1) out += CROCKFORD[rnd[i] & 31];
  return out;
}

function frame(type, payload) {
  return JSON.stringify({ v: 1, t: type, id: makeUlid(), ts: Date.now(), p: payload ?? {} });
}

function signAuth(identity, nonceB64) {
  const message = `claude-monitor-v1|auth|monitor|${identity.deviceId}|${nonceB64}`;
  const signer = createSign('SHA256');
  signer.update(message, 'utf8');
  signer.end();
  return signer.sign({ key: createPrivateKey(identity.signPrivPem), dsaEncoding: 'ieee-p1363' })
               .toString('base64url');
}

function connect(url) {
  return new Promise((resolve, reject) => {
    const ws = new WebSocket(url);
    ws.once('open', () => resolve(ws));
    ws.once('error', reject);
  });
}

function waitFrame(ws, type, timeoutMs = 10000) {
  return new Promise((resolve, reject) => {
    const timer = setTimeout(() => {
      ws.off('message', onMessage);
      reject(new Error(`не дочекався кадру "${type}"`));
    }, timeoutMs);

    function onMessage(data) {
      let parsed;
      try { parsed = JSON.parse(data.toString('utf8')); } catch { return; }
      if (parsed.t !== type) return;
      clearTimeout(timer);
      ws.off('message', onMessage);
      resolve(parsed);
    }
    ws.on('message', onMessage);
  });
}

// ── Команда pair ─────────────────────────────────────────────────────────────

async function commandPair(url, rawCode) {
  const code = rawCode.replace(/[-\s]/g, '').toUpperCase();
  if (code.length !== 12) {
    console.error(`Код має містити 12 символів, отримано ${code.length}.`);
    process.exit(2);
  }

  const identity = makeIdentity();
  console.log(`Ідентифікатор цього монітора: ${identity.deviceId.slice(0, 16)}…`);

  const ws = await connect(url);

  // offer_id порожній: користувач знає лише код. Relay розішле заявку
  // всім активним пропозиціям, а підтвердить її той Bridge, у якого
  // збігся код підтвердження.
  const confirm = computeConfirm(code, 'monitor', identity.pubSig, identity.pubEcdh);

  ws.send(frame('pair_claim', {
    offer_id: '',
    pub_sig: identity.pubSig,
    pub_ecdh: identity.pubEcdh,
    confirm,
  }));

  const response = await waitFrame(ws, 'pair_ok', 30000);
  if (!response.p.accepted) {
    console.error('Bridge відхилив код.');
    process.exit(1);
  }

  const peerPubSig = response.p.peer_pub_sig;
  const peerPubEcdh = response.p.peer_pub_ecdh;
  const peerDeviceId = createHash('sha256')
    .update(Buffer.from(peerPubSig, 'base64url')).digest('hex');

  saveIdentity({ ...identity, peerPubSig, peerPubEcdh, peerDeviceId });

  console.log('Підключення успішне.');
  console.log(`  Ноутбук: ${peerDeviceId.slice(0, 16)}…`);
  ws.close();
}

// ── Команда watch ────────────────────────────────────────────────────────────

function renderSnapshot(snapshot) {
  const s = snapshot.summary ?? {};
  console.log('\n' + '═'.repeat(62));
  console.log(`  CLAUDE MONITOR   ●  ${snapshot.bridge?.host ?? '?'}   ` +
              `Bridge ${snapshot.bridge?.rss_mb?.toFixed(1) ?? '?'} MB / ` +
              `${snapshot.bridge?.cpu_pct?.toFixed(1) ?? '?'}% CPU`);
  console.log('═'.repeat(62));
  console.log(`  АКТИВНИХ ЗАДАЧ: ${s.active ?? 0}    ` +
              `● працює: ${s.working ?? 0}   ` +
              `◐ чекає: ${s.waiting ?? 0}   ` +
              `✓ завершено: ${s.finished ?? 0}   ` +
              `✕ помилок: ${s.error ?? 0}`);
  console.log('─'.repeat(62));

  for (const session of snapshot.sessions ?? []) {
    const icon = { working: '●', waiting: '◐', idle: '○', error: '✕' }[session.state] ?? '·';
    const elapsed = session.started_at
      ? Math.floor((Date.now() - session.started_at) / 1000) : 0;
    const mm = String(Math.floor(elapsed / 60)).padStart(2, '0');
    const ss = String(elapsed % 60).padStart(2, '0');

    console.log(`  [${session.short}] ${icon} ${session.state.toUpperCase()}`);
    console.log(`      ${session.title}   (джерело назви: ${session.title_src})`);
    console.log(`      проєкт: ${session.project}${session.branch ? '  гілка: ' + session.branch : ''}`);
    if (session.activity) {
      console.log(`      ├─ ${session.activity.action}: ${session.activity.target ?? ''}`);
    }
    console.log(`      └─ ${mm}:${ss}`);
    console.log('');
  }
  console.log('═'.repeat(62) + '\n');
}

function renderEvents(items) {
  for (const item of items) {
    const time = new Date(item.ts).toLocaleTimeString('uk-UA');
    const tag = `[${item.sid.slice(0, 8)}]`;

    let text = '';
    if (item.k === 'activity') text = `${item.action}  ${item.target ?? ''}`;
    else if (item.k === 'status') text = `СТАН -> ${item.state}${item.text ? ': ' + item.text : ''}`;
    else if (item.k === 'result') text = `${item.status === 'error' ? '✕' : '✓'} ${item.text ?? ''}`;
    else if (item.k === 'session') text = `СЕСІЯ ${item.event} (${item.state})`;
    else text = item.text ?? '';

    console.log(`${time}  ${tag}  ${text.slice(0, 100)}`);
  }
}

async function commandWatch(url) {
  const identity = loadIdentity();
  if (!identity?.peerDeviceId) {
    console.error('Спершу виконайте pair.');
    process.exit(2);
  }

  const sessionKey = deriveSessionKey(identity, identity.peerPubEcdh,
                                      identity.deviceId, identity.peerDeviceId);

  let attempt = 0;
  for (;;) {
    try {
      const ws = await connect(url);
      ws.send(frame('hello', { role: 'monitor', device_id: identity.deviceId, client: 'test-monitor/1.0' }));

      const challenge = await waitFrame(ws, 'challenge');
      ws.send(frame('auth', { device_id: identity.deviceId, sig: signAuth(identity, challenge.p.nonce) }));

      const authOk = await waitFrame(ws, 'auth_ok');
      attempt = 0;
      console.log(`Підключено. Ноутбук ${authOk.p.peer_online ? 'у мережі' : 'поза мережею'}.`);

      // Просимо повний стан одразу після автентифікації.
      const request = encryptPayload(sessionKey, JSON.stringify({ t: 'snapshot_request', since_seq: 0 }));
      ws.send(frame('fwd', request));

      await new Promise((resolve) => {
        ws.on('message', (data) => {
          let parsed;
          try { parsed = JSON.parse(data.toString('utf8')); } catch { return; }

          if (parsed.t === 'ping') { ws.send(frame('pong', {})); return; }
          if (parsed.t === 'error') {
            console.log(`[relay] ${parsed.p.code}`);
            return;
          }
          if (parsed.t !== 'fwd') return;

          let plain;
          try {
            plain = decryptPayload(sessionKey, parsed.p.n, parsed.p.ct);
          } catch (err) {
            console.error(`не вдалося розшифрувати: ${err.message}`);
            return;
          }

          const message = JSON.parse(plain);
          if (message.t === 'snapshot') renderSnapshot(message);
          else if (message.t === 'events') renderEvents(message.items ?? []);
        });

        ws.on('close', resolve);
        ws.on('error', resolve);
      });

      console.log('З\'єднання втрачено.');
    } catch (err) {
      console.error(`помилка: ${err.message}`);
    }

    // Той самий backoff, що й у Bridge: 1, 2, 4, 8, 16, 30, 60 с.
    const delay = Math.min(1000 * 2 ** Math.min(attempt, 5), 60000);
    attempt += 1;
    console.log(`перепідключення через ${delay / 1000} с…`);
    await new Promise((r) => setTimeout(r, delay));
  }
}

// ── Точка входу ──────────────────────────────────────────────────────────────

const [command, url, code] = process.argv.slice(2);

if (command === 'pair' && url && code) {
  await commandPair(url, code);
} else if (command === 'watch' && url) {
  await commandWatch(url);
} else {
  console.log('Використання:');
  console.log('  node scripts/test-monitor.mjs pair  <relay-url> <код>');
  console.log('  node scripts/test-monitor.mjs watch <relay-url>');
  process.exit(2);
}
