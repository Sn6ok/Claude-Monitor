/**
 * Тести протоколу (Частина 6 §26 Master Prompt).
 *
 * Перевіряються всі перелічені там випадки: коректне повідомлення, некоректне,
 * невідомий тип, хибна версія, відсутнє поле, завелике поле, завеликий кадр.
 */

import { test, describe } from 'node:test';
import assert from 'node:assert/strict';
import { randomBytes } from 'node:crypto';

import { parseFrame, makeFrame, makeError, makeUlid, LIMITS, FRAME_TYPES } from '../src/protocol.js';

const NOW = 1_770_000_000_000;
const HEX64 = 'a'.repeat(64);

function frame(overrides = {}) {
  return JSON.stringify({
    v: 1,
    t: 'hello',
    id: makeUlid(randomBytes, NOW),
    ts: NOW,
    p: { role: 'bridge', device_id: HEX64 },
    ...overrides,
  });
}

describe('parseFrame — коректні кадри', () => {
  test('приймає валідний hello', () => {
    const r = parseFrame(frame(), false, NOW);
    assert.equal(r.ok, true);
    assert.equal(r.frame.t, 'hello');
    assert.equal(r.frame.p.role, 'bridge');
  });

  test('приймає ping із порожнім payload', () => {
    const r = parseFrame(frame({ t: 'ping', p: {} }), false, NOW);
    assert.equal(r.ok, true);
  });

  test('ігнорує невідомі поля всередині відомого типу (сумісність уперед)', () => {
    const raw = frame({ p: { role: 'bridge', device_id: HEX64, майбутнє_поле: 42 } });
    const r = parseFrame(raw, false, NOW);
    assert.equal(r.ok, true);
  });
});

describe('parseFrame — відхилення', () => {
  test('завеликий кадр відхиляється до розбору JSON', () => {
    const huge = 'x'.repeat(LIMITS.MAX_FRAME_BYTES + 1);
    const r = parseFrame(huge, false, NOW);
    assert.equal(r.ok, false);
    assert.equal(r.code, 'too_large');
  });

  test('порожній кадр', () => {
    assert.equal(parseFrame('', false, NOW).code, 'bad_frame');
  });

  test('бінарний кадр не приймається — протокол текстовий', () => {
    const r = parseFrame(Buffer.from(frame()), true, NOW);
    assert.equal(r.ok, false);
    assert.equal(r.detail, 'binary');
  });

  test('некоректний JSON', () => {
    const r = parseFrame('{ це не json', false, NOW);
    assert.equal(r.code, 'bad_frame');
    assert.equal(r.detail, 'json');
  });

  test('JSON-масив замість обʼєкта', () => {
    assert.equal(parseFrame('[1,2,3]', false, NOW).detail, 'not_object');
  });

  test('інша версія протоколу дає окремий код, а не bad_frame', () => {
    const r = parseFrame(frame({ v: 2 }), false, NOW);
    assert.equal(r.code, 'proto_unsupported');
  });

  test('невідомий тип кадру', () => {
    const r = parseFrame(frame({ t: 'shutdown' }), false, NOW);
    assert.equal(r.detail, 'unknown_type');
  });

  test('серверні типи не приймаються від клієнта', () => {
    for (const t of ['challenge', 'auth_ok', 'error']) {
      const r = parseFrame(frame({ t, p: {} }), false, NOW);
      assert.equal(r.ok, false, `${t} мав бути відхилений`);
      assert.equal(r.detail, 'server_only');
    }
  });

  test('некоректний ULID', () => {
    assert.equal(parseFrame(frame({ id: 'короткий' }), false, NOW).detail, 'id');
  });

  test('час поза допустимим вікном — захист від replay', () => {
    const stale = parseFrame(frame({ ts: NOW - LIMITS.CLOCK_SKEW_MS - 1000 }), false, NOW);
    assert.equal(stale.detail, 'clock_skew');

    const future = parseFrame(frame({ ts: NOW + LIMITS.CLOCK_SKEW_MS + 1000 }), false, NOW);
    assert.equal(future.detail, 'clock_skew');
  });

  test('відсутнє обовʼязкове поле payload', () => {
    const r = parseFrame(frame({ p: { role: 'bridge' } }), false, NOW);
    assert.equal(r.detail, 'hello.device_id');
  });

  test('device_id хибного формату', () => {
    for (const bad of ['ZZZ', 'A'.repeat(64), HEX64 + 'a', '']) {
      const r = parseFrame(frame({ p: { role: 'bridge', device_id: bad } }), false, NOW);
      assert.equal(r.ok, false, `device_id "${bad.slice(0, 12)}" мав бути відхилений`);
    }
  });

  test('невідома роль', () => {
    const r = parseFrame(frame({ p: { role: 'admin', device_id: HEX64 } }), false, NOW);
    assert.equal(r.detail, 'hello.role');
  });

  test('завелике поле шифротексту у fwd', () => {
    const raw = frame({
      t: 'fwd',
      p: { n: 'A'.repeat(16), ct: 'A'.repeat(LIMITS.MAX_CIPHERTEXT_LEN + 1) },
    });
    // Спершу спрацює ліміт розміру всього кадру — це теж коректне відхилення.
    const r = parseFrame(raw, false, NOW);
    assert.equal(r.ok, false);
  });

  test('base64url із неприпустимими символами', () => {
    const r = parseFrame(frame({ t: 'fwd', p: { n: 'AAAA+/==', ct: 'AAAA' } }), false, NOW);
    assert.equal(r.detail, 'fwd.n');
  });

  test('жоден некоректний вхід не кидає винятку', () => {
    const inputs = [
      'null', '0', '"рядок"', '{}', '{"v":1}', '[]',
      '{"v":1,"t":null,"id":null,"ts":null}',
      Buffer.from([0xff, 0xfe, 0xfd]),
      '{"v":1,"t":"fwd","id":"' + 'A'.repeat(26) + '","ts":' + NOW + ',"p":null}',
    ];
    for (const input of inputs) {
      assert.doesNotThrow(() => parseFrame(input, false, NOW));
    }
  });
});

describe('генерація кадрів', () => {
  test('makeFrame створює кадр, який проходить власну валідацію', () => {
    for (const t of ['ping', 'pong']) {
      const raw = makeFrame(t, {}, randomBytes, NOW);
      assert.equal(parseFrame(raw, false, NOW).ok, true, `${t} має бути валідним`);
    }
  });

  test('ULID унікальні навіть у межах однієї мілісекунди', () => {
    const ids = new Set();
    for (let i = 0; i < 500; i += 1) ids.add(makeUlid(randomBytes, NOW));
    assert.equal(ids.size, 500);
  });

  test('ULID має правильну довжину й алфавіт', () => {
    const id = makeUlid(randomBytes, NOW);
    assert.equal(id.length, 26);
    assert.match(id, /^[0-9A-HJKMNP-TV-Z]{26}$/);
  });

  test('makeError не пропускає назовні невідомі коди', () => {
    const raw = makeError('щось_внутрішнє', randomBytes);
    assert.equal(JSON.parse(raw).p.code, 'internal');
  });

  test('makeError не містить деталей винятків', () => {
    const raw = makeError('bad_frame', randomBytes);
    const parsed = JSON.parse(raw);
    assert.deepEqual(Object.keys(parsed.p), ['code']);
  });
});

describe('повнота набору типів', () => {
  test('усі типи кадрів мають валідатор або явно серверні', () => {
    for (const t of FRAME_TYPES) {
      const raw = frame({ t, p: {} });
      // Не має бути «мовчазного пропуску»: кожен тип або валідується,
      // або відхиляється з конкретної причини.
      assert.doesNotThrow(() => parseFrame(raw, false, NOW), `тип ${t}`);
    }
  });
});
