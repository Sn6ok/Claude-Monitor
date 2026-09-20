/**
 * Допоміжний тестовий клієнт.
 *
 * Реалізує ту саму криптографію, що й справжні Bridge та Android:
 * ECDSA P-256 у форматі IEEE P1363, ідентифікатор пристрою як SHA-256
 * від сирої точки публічного ключа.
 */

import { generateKeyPairSync, createSign, createHash, randomBytes } from 'node:crypto';
import { WebSocket } from 'ws';
import { makeFrame } from '../src/protocol.js';
import { authSigningString } from '../src/crypto.js';

/** Створює пару ключів P-256 і похідний device_id. */
export function makeIdentity() {
  const { publicKey, privateKey } = generateKeyPairSync('ec', { namedCurve: 'prime256v1' });

  // JWK -> сира неспресована точка 0x04 || X || Y, як це роблять
  // Windows CNG та Android Keystore.
  const jwk = publicKey.export({ format: 'jwk' });
  const rawPoint = Buffer.concat([
    Buffer.from([0x04]),
    Buffer.from(jwk.x, 'base64url'),
    Buffer.from(jwk.y, 'base64url'),
  ]);

  return {
    privateKey,
    publicKey,
    pubSig: rawPoint.toString('base64url'),
    pubEcdh: rawPoint.toString('base64url'), // у тестах достатньо тієї самої точки
    deviceId: createHash('sha256').update(rawPoint).digest('hex'),
  };
}

export function signAuth(identity, role, nonce) {
  const signer = createSign('SHA256');
  signer.update(authSigningString(role, identity.deviceId, nonce), 'utf8');
  signer.end();
  return signer
    .sign({ key: identity.privateKey, dsaEncoding: 'ieee-p1363' })
    .toString('base64url');
}

/** Тонка обгортка навколо WebSocket із очікуванням конкретних кадрів. */
export class TestClient {
  constructor(url) {
    this.url = url;
    this.ws = null;
    this.received = [];
    this.waiters = [];
    this.closed = false;
    this.closeCode = null;
  }

  connect() {
    return new Promise((resolve, reject) => {
      this.ws = new WebSocket(this.url);
      this.ws.on('open', resolve);
      this.ws.on('error', reject);
      this.ws.on('message', (data) => {
        let frame;
        try {
          frame = JSON.parse(data.toString('utf8'));
        } catch {
          frame = { t: '<не json>', raw: data.toString('utf8') };
        }
        this.received.push(frame);
        for (let i = this.waiters.length - 1; i >= 0; i -= 1) {
          const waiter = this.waiters[i];
          if (waiter.match(frame)) {
            this.waiters.splice(i, 1);
            waiter.resolve(frame);
          }
        }
      });
      this.ws.on('close', (code) => {
        this.closed = true;
        this.closeCode = code;
        for (const waiter of this.waiters.splice(0)) {
          waiter.reject(new Error(`зʼєднання закрито (код ${code})`));
        }
      });
    });
  }

  send(type, payload) {
    this.ws.send(makeFrame(type, payload, randomBytes));
  }

  sendRaw(data) {
    this.ws.send(data);
  }

  /** Чекає на кадр заданого типу. Уже отримані кадри теж враховуються. */
  waitFor(type, timeoutMs = 3000) {
    const match = (f) => f.t === type;
    const already = this.received.find(match);
    if (already) return Promise.resolve(already);

    return new Promise((resolve, reject) => {
      const timer = setTimeout(() => {
        const idx = this.waiters.findIndex((w) => w.timer === timer);
        if (idx !== -1) this.waiters.splice(idx, 1);
        reject(new Error(`очікування кадру "${type}" вичерпало ${timeoutMs} мс`));
      }, timeoutMs);
      timer.unref?.();

      this.waiters.push({
        match,
        timer,
        resolve: (f) => { clearTimeout(timer); resolve(f); },
        reject: (e) => { clearTimeout(timer); reject(e); },
      });
    });
  }

  waitForClose(timeoutMs = 3000) {
    if (this.closed) return Promise.resolve(this.closeCode);
    return new Promise((resolve, reject) => {
      const timer = setTimeout(() => reject(new Error('зʼєднання не закрилось вчасно')), timeoutMs);
      timer.unref?.();
      this.ws.on('close', (code) => { clearTimeout(timer); resolve(code); });
    });
  }

  /** Повний цикл рукостискання: hello -> challenge -> auth -> auth_ok. */
  async authenticate(identity, role) {
    this.send('hello', { role, device_id: identity.deviceId, client: 'test/1.0.0' });
    const challenge = await this.waitFor('challenge');
    this.send('auth', {
      device_id: identity.deviceId,
      sig: signAuth(identity, role, challenge.p.nonce),
    });
    return this.waitFor('auth_ok');
  }

  close() {
    try { this.ws?.close(); } catch { /* уже закрито */ }
  }
}

/** Пауза — для перевірок, що залежать від часу. */
export const sleep = (ms) => new Promise((r) => setTimeout(r, ms).unref?.());
