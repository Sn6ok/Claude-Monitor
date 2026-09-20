/**
 * Валідація кадрів протоколу Claude Monitor v1.
 *
 * Правило цього модуля: він НІКОЛИ не довіряє вхідним даним і ніколи не кидає
 * винятків назовні. Будь-який некоректний вхід повертає { ok: false, code }.
 *
 * Порядок перевірок навмисно йде від найдешевших до найдорожчих — кадр на
 * 10 МБ має бути відхилений до того, як його побачить JSON.parse.
 * Специфікація: docs/protocol.md §11.
 */

export const PROTOCOL_VERSION = 1;

/** Ліміти з docs/protocol.md §2. */
export const LIMITS = Object.freeze({
  MAX_FRAME_BYTES: 65536,
  MAX_EVENT_TEXT: 512,
  MAX_EVENTS_PER_BATCH: 32,
  MAX_SESSIONS: 16,
  CLOCK_SKEW_MS: 300_000,

  MAX_TYPE_LEN: 24,
  ULID_LEN: 26,
  MAX_DEVICE_ID_LEN: 64,
  MAX_CLIENT_LEN: 64,
  MAX_B64_LEN: 4096,
  MAX_CIPHERTEXT_LEN: 65536,
  MAX_OFFER_ID_LEN: 32,
});

/** Типи кадрів рівня 1. Невідомий тип відхиляється (docs/protocol.md §9). */
export const FRAME_TYPES = Object.freeze([
  'hello',
  'challenge',
  'auth',
  'auth_ok',
  'fwd',
  'ping',
  'pong',
  'error',
  'pair_offer',
  'pair_claim',
  'pair_ok',
  'pair_cancel',
]);

export const ROLES = Object.freeze(['bridge', 'monitor']);

export const ERROR_CODES = Object.freeze([
  'bad_frame',
  'proto_unsupported',
  'auth_failed',
  'device_revoked',
  'rate_limited',
  'peer_offline',
  'too_large',
  'not_paired',
  'internal',
]);

const RE_ULID = /^[0-9A-HJKMNP-TV-Z]{26}$/;
const RE_HEX64 = /^[0-9a-f]{64}$/;
const RE_TYPE = /^[a-z_.]{1,24}$/;
const RE_B64URL = /^[A-Za-z0-9_-]+$/;
const RE_CLIENT = /^[A-Za-z0-9._/-]{1,64}$/;
const RE_OFFER_ID = /^[0-9a-f]{32}$/;

const fail = (code, detail) => ({ ok: false, code, detail });
const pass = (frame) => ({ ok: true, frame });

/** Рядок заданої довжини у base64url — саме так кодуються всі бінарні поля. */
function isB64Url(value, maxLen = LIMITS.MAX_B64_LEN) {
  return (
    typeof value === 'string' &&
    value.length > 0 &&
    value.length <= maxLen &&
    RE_B64URL.test(value)
  );
}

function isPlainObject(value) {
  return typeof value === 'object' && value !== null && !Array.isArray(value);
}

/**
 * Розбирає та валідує вхідний кадр.
 *
 * @param {Buffer|ArrayBuffer|string} raw     сирі дані з WebSocket
 * @param {boolean}                   isBinary чи прийшов кадр як бінарний
 * @param {number}                    nowMs    поточний час (інʼєкція для тестів)
 */
export function parseFrame(raw, isBinary, nowMs = Date.now()) {
  // 1. Розмір — до будь-якого розбору.
  const byteLength =
    typeof raw === 'string' ? Buffer.byteLength(raw, 'utf8') : raw.byteLength ?? raw.length ?? 0;

  if (byteLength > LIMITS.MAX_FRAME_BYTES) return fail('too_large');
  if (byteLength === 0) return fail('bad_frame', 'empty');

  // Протокол текстовий. Бінарні кадри не передбачені й не приймаються.
  if (isBinary) return fail('bad_frame', 'binary');

  // 2. Коректний UTF-8 JSON.
  let frame;
  try {
    frame = JSON.parse(typeof raw === 'string' ? raw : Buffer.from(raw).toString('utf8'));
  } catch {
    return fail('bad_frame', 'json');
  }
  if (!isPlainObject(frame)) return fail('bad_frame', 'not_object');

  // 3. Версія протоколу. Інша версія — окремий код, щоб клієнт не
  //    перепідключався в циклі, а показав «оновіть застосунок».
  if (frame.v !== PROTOCOL_VERSION) return fail('proto_unsupported');

  // 4. Тип кадру.
  if (typeof frame.t !== 'string' || !RE_TYPE.test(frame.t)) return fail('bad_frame', 'type');
  if (!FRAME_TYPES.includes(frame.t)) return fail('bad_frame', 'unknown_type');

  // 5. Ідентифікатор кадру.
  if (typeof frame.id !== 'string' || !RE_ULID.test(frame.id)) return fail('bad_frame', 'id');

  // 6. Час відправника у допустимому вікні (захист від replay, §10 security).
  if (!Number.isSafeInteger(frame.ts)) return fail('bad_frame', 'ts');
  if (Math.abs(frame.ts - nowMs) > LIMITS.CLOCK_SKEW_MS) return fail('bad_frame', 'clock_skew');

  // 7-8. Форма корисного навантаження для конкретного типу.
  const payload = frame.p ?? {};
  if (!isPlainObject(payload)) return fail('bad_frame', 'payload');

  const check = PAYLOAD_VALIDATORS[frame.t];
  const detail = check ? check(payload) : null;
  if (detail) return fail('bad_frame', detail);

  return pass({ v: frame.v, t: frame.t, id: frame.id, ts: frame.ts, p: payload });
}

/**
 * Валідатори корисного навантаження. Кожен повертає рядок-причину помилки
 * або null, якщо все гаразд.
 */
const PAYLOAD_VALIDATORS = {
  hello(p) {
    if (!ROLES.includes(p.role)) return 'hello.role';
    if (typeof p.device_id !== 'string' || !RE_HEX64.test(p.device_id)) return 'hello.device_id';
    if (p.client !== undefined && (typeof p.client !== 'string' || !RE_CLIENT.test(p.client)))
      return 'hello.client';
    return null;
  },

  auth(p) {
    if (typeof p.device_id !== 'string' || !RE_HEX64.test(p.device_id)) return 'auth.device_id';
    if (!isB64Url(p.sig, 256)) return 'auth.sig';
    return null;
  },

  fwd(p) {
    // 12 байт nonce -> 16 символів base64url без padding.
    if (!isB64Url(p.n, 32)) return 'fwd.n';
    if (!isB64Url(p.ct, LIMITS.MAX_CIPHERTEXT_LEN)) return 'fwd.ct';
    // Адресат — необов'язковий: Bridge вказує його, коли телефонів кілька.
    if (p.to !== undefined && (typeof p.to !== 'string' || !RE_HEX64.test(p.to))) return 'fwd.to';
    return null;
  },

  pair_offer(p) {
    if (typeof p.offer_id !== 'string' || !RE_OFFER_ID.test(p.offer_id)) return 'pair_offer.offer_id';
    if (!isB64Url(p.pub_sig, 256)) return 'pair_offer.pub_sig';
    if (!isB64Url(p.pub_ecdh, 256)) return 'pair_offer.pub_ecdh';
    if (!isB64Url(p.confirm, 128)) return 'pair_offer.confirm';
    return null;
  },

  pair_claim(p) {
    // Порожній offer_id дозволений: користувач вводить лише код, а
    // ідентифікатора пропозиції не знає. Relay тоді розсилає заявку всім
    // активним пропозиціям, і підтвердить її лише той Bridge, у якого
    // збігся код. Безпеки це не послаблює — перевірка все одно
    // відбувається за HMAC, якого Relay обчислити не може.
    if (typeof p.offer_id !== 'string') return 'pair_claim.offer_id';
    if (p.offer_id.length > 0 && !RE_OFFER_ID.test(p.offer_id)) return 'pair_claim.offer_id';
    if (!isB64Url(p.pub_sig, 256)) return 'pair_claim.pub_sig';
    if (!isB64Url(p.pub_ecdh, 256)) return 'pair_claim.pub_ecdh';
    if (!isB64Url(p.confirm, 128)) return 'pair_claim.confirm';
    return null;
  },

  pair_ok(p) {
    if (typeof p.offer_id !== 'string' || !RE_OFFER_ID.test(p.offer_id)) return 'pair_ok.offer_id';
    if (typeof p.accepted !== 'boolean') return 'pair_ok.accepted';
    return null;
  },

  pair_cancel(p) {
    if (typeof p.offer_id !== 'string' || !RE_OFFER_ID.test(p.offer_id)) return 'pair_cancel.offer_id';
    return null;
  },

  // Кадри, які Relay лише надсилає, але приймати від клієнта не має права.
  challenge: () => 'server_only',
  auth_ok: () => 'server_only',
  error: () => 'server_only',

  ping: () => null,
  pong: () => null,
};

/** Монотонний лічильник, щоб ULID у межах однієї мілісекунди не повторювались. */
let ulidCounter = 0;
let ulidLastMs = 0;

const CROCKFORD = '0123456789ABCDEFGHJKMNPQRSTVWXYZ';

/**
 * ULID: 10 символів часу + 16 символів випадковості.
 * Використовується лише як ідентифікатор кадру, тож не потребує
 * криптографічної стійкості — але й не має колізій у межах мілісекунди.
 */
export function makeUlid(randomBytes, nowMs = Date.now()) {
  if (nowMs === ulidLastMs) {
    ulidCounter += 1;
  } else {
    ulidLastMs = nowMs;
    ulidCounter = 0;
  }

  let time = nowMs;
  let out = '';
  for (let i = 0; i < 10; i += 1) {
    out = CROCKFORD[time % 32] + out;
    time = Math.floor(time / 32);
  }

  const rnd = randomBytes(10);
  rnd[9] = (rnd[9] + ulidCounter) & 0xff;
  for (let i = 0; i < 16; i += 1) {
    const bitPos = i * 5;
    const byteIdx = bitPos >> 3;
    const shift = bitPos & 7;
    const chunk = ((rnd[byteIdx] << 8) | (rnd[byteIdx + 1] ?? 0)) >> (11 - shift);
    out += CROCKFORD[chunk & 31];
  }
  return out;
}

/** Збирає кадр протоколу для відправлення. */
export function makeFrame(type, payload, randomBytes, nowMs = Date.now()) {
  return JSON.stringify({
    v: PROTOCOL_VERSION,
    t: type,
    id: makeUlid(randomBytes, nowMs),
    ts: nowMs,
    p: payload ?? {},
  });
}

/**
 * Кадр помилки. `detail` навмисно НЕ потрапляє до клієнта —
 * назовні йде лише стабільний код (docs/protocol.md §3.7).
 */
export function makeError(code, randomBytes, retryAfterSec) {
  const safeCode = ERROR_CODES.includes(code) ? code : 'internal';
  const payload = { code: safeCode };
  if (Number.isInteger(retryAfterSec) && retryAfterSec > 0) payload.retry_after = retryAfterSec;
  return makeFrame('error', payload, randomBytes);
}
