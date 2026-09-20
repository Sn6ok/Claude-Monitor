/**
 * Криптографічні операції Relay.
 *
 * Relay виконує рівно одну криптографічну функцію: перевіряє, що пристрій
 * володіє приватним ключем, який відповідає зареєстрованому публічному.
 * Він НЕ шифрує і НЕ дешифрує корисне навантаження — вміст `fwd.ct`
 * зашифрований наскрізно між Bridge і Android (docs/architecture.md §8).
 *
 * Уся криптографія — вбудований модуль node:crypto. Власних алгоритмів немає
 * (Частина 3 §13, §25 Master Prompt).
 */

import { createHash, createPublicKey, createVerify, randomBytes, timingSafeEqual } from 'node:crypto';

/**
 * Формат підпису — IEEE P1363, тобто сирі r||s по 32 байти.
 *
 * Обрано тому, що Windows CNG (BCryptSignHash) віддає підпис саме в такому
 * вигляді нативно, а Android має "SHA256withECDSAinP1363Format" починаючи
 * з API 24. Формат DER потребував би ручної конвертації на обох платформах.
 */
const SIGNATURE_ENCODING = 'ieee-p1363';

/** Довжина неспресованої точки P-256: 0x04 || X(32) || Y(32). */
const EC_POINT_LEN = 65;
const EC_SIGNATURE_LEN = 64;

export function b64urlToBuffer(value) {
  return Buffer.from(value, 'base64url');
}

export function bufferToB64url(buf) {
  return Buffer.from(buf).toString('base64url');
}

/**
 * Перетворює сиру точку P-256 на об'єкт ключа Node через JWK.
 * Повертає null для будь-яких некоректних даних — виняток назовні не летить.
 */
export function publicKeyFromRawPoint(rawB64Url) {
  try {
    const raw = b64urlToBuffer(rawB64Url);
    if (raw.length !== EC_POINT_LEN || raw[0] !== 0x04) return null;

    return createPublicKey({
      key: {
        kty: 'EC',
        crv: 'P-256',
        x: raw.subarray(1, 33).toString('base64url'),
        y: raw.subarray(33, 65).toString('base64url'),
      },
      format: 'jwk',
    });
  } catch {
    return null;
  }
}

/**
 * Ідентифікатор пристрою = SHA-256 від сирої точки публічного ключа підпису.
 *
 * Похідність від ключа означає, що пристрій не може заявити чужий
 * device_id — він не зміг би підтвердити його підписом.
 */
export function deviceIdFromPublicKey(rawB64Url) {
  const raw = b64urlToBuffer(rawB64Url);
  if (raw.length !== EC_POINT_LEN || raw[0] !== 0x04) return null;
  return createHash('sha256').update(raw).digest('hex');
}

/**
 * Рядок, який підписує пристрій під час автентифікації.
 *
 * Префікс із назвою та версією протоколу — це domain separation: підпис,
 * зроблений для автентифікації, не можна перевикористати в іншому контексті
 * (docs/protocol.md §3.3).
 */
export function authSigningString(role, deviceId, nonceB64Url) {
  return `claude-monitor-v1|auth|${role}|${deviceId}|${nonceB64Url}`;
}

/**
 * Перевіряє підпис виклику.
 * @returns {boolean} true лише за повністю коректного підпису
 */
export function verifyAuthSignature({ role, deviceId, nonceB64Url, signatureB64Url, publicKeyRawB64Url }) {
  try {
    const signature = b64urlToBuffer(signatureB64Url);
    if (signature.length !== EC_SIGNATURE_LEN) return false;

    const key = publicKeyFromRawPoint(publicKeyRawB64Url);
    if (!key) return false;

    const verifier = createVerify('SHA256');
    verifier.update(authSigningString(role, deviceId, nonceB64Url), 'utf8');
    verifier.end();

    return verifier.verify({ key, dsaEncoding: SIGNATURE_ENCODING }, signature);
  } catch {
    // Некоректний ключ, підпис чи кодування — це просто невдала перевірка.
    return false;
  }
}

/** Криптографічно стійкий nonce виклику (32 байти). */
export function makeChallengeNonce() {
  return randomBytes(32).toString('base64url');
}

/** Непрогнозований ідентифікатор пропозиції pairing (docs/protocol.md §4). */
export function makeOfferId() {
  return randomBytes(16).toString('hex');
}

/** Порівняння без витоку часу — для порівняння токенів і хешів. */
export function constantTimeEqual(a, b) {
  const bufA = Buffer.from(String(a), 'utf8');
  const bufB = Buffer.from(String(b), 'utf8');
  if (bufA.length !== bufB.length) return false;
  return timingSafeEqual(bufA, bufB);
}

export { randomBytes };
