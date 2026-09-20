/**
 * Реєстр пристроїв Relay.
 *
 * Що тут зберігається: публічні ключі, ролі, звʼязки pairing, стан відкликання.
 * Чого тут немає і ніколи не буде: приватних ключів, подій Claude Code,
 * вмісту повідомлень, вихідного коду, облікових даних Claude
 * (Частина 3 §15, Частина 6 §8 Master Prompt).
 *
 * Реєстр — єдиний стан, який Relay зберігає між перезапусками. Усе інше
 * (з'єднання, виклики, пропозиції pairing) живе лише в пам'яті.
 */

import { readFileSync, writeFileSync, renameSync, existsSync, mkdirSync, chmodSync } from 'node:fs';
import { dirname } from 'node:path';

const STORE_VERSION = 1;

export class DeviceRegistry {
  /**
   * @param {string} filePath шлях до файлу реєстру
   * @param {object} [logger]
   */
  constructor(filePath, logger = console) {
    this.filePath = filePath;
    this.logger = logger;
    /** @type {Map<string, object>} */
    this.devices = new Map();
    this.load();
  }

  load() {
    if (!existsSync(this.filePath)) {
      this.devices = new Map();
      return;
    }
    try {
      const parsed = JSON.parse(readFileSync(this.filePath, 'utf8'));
      if (parsed?.version !== STORE_VERSION || typeof parsed.devices !== 'object') {
        throw new Error('несумісний формат реєстру');
      }
      this.devices = new Map(Object.entries(parsed.devices));
    } catch (err) {
      // Пошкоджений реєстр не повинен валити сервер. Він відкладається вбік,
      // а Relay стартує з порожнім реєстром: пристрої просто пройдуть pairing
      // повторно. Мовчки продовжити з невідомим станом було б гірше.
      const backup = `${this.filePath}.corrupt-${Date.now()}`;
      try {
        renameSync(this.filePath, backup);
        this.logger.error(`[devices] реєстр пошкоджено, відкладено у ${backup}: ${err.message}`);
      } catch {
        this.logger.error(`[devices] реєстр пошкоджено і не вдалося зберегти копію: ${err.message}`);
      }
      this.devices = new Map();
    }
  }

  /**
   * Атомарний запис: спершу тимчасовий файл, потім перейменування.
   * Так обрив живлення посеред запису не залишає напівзаписаний реєстр.
   */
  persist() {
    const dir = dirname(this.filePath);
    if (!existsSync(dir)) mkdirSync(dir, { recursive: true, mode: 0o700 });

    const payload = JSON.stringify(
      { version: STORE_VERSION, devices: Object.fromEntries(this.devices) },
      null,
      2,
    );

    const tmp = `${this.filePath}.tmp`;
    writeFileSync(tmp, payload, { mode: 0o600 });
    renameSync(tmp, this.filePath);

    // На Windows chmod не має ефекту, тому помилка тут не критична.
    try {
      chmodSync(this.filePath, 0o600);
    } catch {
      /* Windows: права керуються ACL, а файл лежить у профілі користувача */
    }
  }

  get(deviceId) {
    return this.devices.get(deviceId) ?? null;
  }

  has(deviceId) {
    return this.devices.has(deviceId);
  }

  /** Пристрій вважається дійсним, лише якщо він відомий і не відкликаний. */
  isActive(deviceId) {
    const device = this.devices.get(deviceId);
    return Boolean(device) && device.revoked !== true;
  }

  /**
   * Реєструє пристрій або оновлює вже наявний.
   * Ключі наявного пристрою НЕ перезаписуються: інакше той, хто якось
   * дізнався чужий device_id, міг би підмінити ключі й перехопити доступ.
   */
  upsert({ deviceId, role, pubSig, pubEcdh, label }, nowMs = Date.now()) {
    const existing = this.devices.get(deviceId);

    if (existing) {
      if (existing.pub_sig !== pubSig || existing.pub_ecdh !== pubEcdh) {
        return { ok: false, reason: 'key_mismatch' };
      }
      existing.last_seen = nowMs;
      if (label) existing.label = String(label).slice(0, 64);
      this.persist();
      return { ok: true, device: existing, created: false };
    }

    const device = {
      role,
      pub_sig: pubSig,
      pub_ecdh: pubEcdh,
      label: label ? String(label).slice(0, 64) : role,
      created_at: nowMs,
      last_seen: nowMs,
      revoked: false,
      paired_with: [],
    };
    this.devices.set(deviceId, device);
    this.persist();
    return { ok: true, device, created: true };
  }

  /** Взаємно звʼязує два пристрої. Звʼязок завжди двосторонній. */
  pair(deviceIdA, deviceIdB) {
    const a = this.devices.get(deviceIdA);
    const b = this.devices.get(deviceIdB);
    if (!a || !b) return false;

    if (!a.paired_with.includes(deviceIdB)) a.paired_with.push(deviceIdB);
    if (!b.paired_with.includes(deviceIdA)) b.paired_with.push(deviceIdA);
    this.persist();
    return true;
  }


  /**
   * Чи має пристрій право надсилати повідомлення іншому.
   *
   * Перевіряються всі умови одразу: обидва відомі, жоден не відкликаний,
   * звʼязок існує в обох напрямках і ролі різні. Знання чужого device_id
   * саме по собі не дає жодного доступу (Частина 3 §11).
   */
  isAuthorizedPair(fromId, toId) {
    if (fromId === toId) return false;
    const from = this.devices.get(fromId);
    const to = this.devices.get(toId);
    if (!from || !to) return false;
    if (from.revoked || to.revoked) return false;
    if (from.role === to.role) return false;
    return from.paired_with.includes(toId) && to.paired_with.includes(fromId);
  }

  /** Знаходить пристрої-партнери заданої ролі. */
  peersOf(deviceId, role) {
    const device = this.devices.get(deviceId);
    if (!device) return [];
    return device.paired_with.filter((id) => {
      const peer = this.devices.get(id);
      return peer && !peer.revoked && peer.role === role;
    });
  }

  /**
   * Відкликає пристрій (Частина 3 §21, §22 — сценарій втраченого телефона).
   * Запис не видаляється: збережений відкликаний device_id гарантує, що той
   * самий ключ не зможе просто пройти pairing заново без відома користувача.
   */
  revoke(deviceId, nowMs = Date.now()) {
    const device = this.devices.get(deviceId);
    if (!device) return false;
    device.revoked = true;
    device.revoked_at = nowMs;
    device.paired_with = [];

    for (const other of this.devices.values()) {
      const idx = other.paired_with.indexOf(deviceId);
      if (idx !== -1) other.paired_with.splice(idx, 1);
    }
    this.persist();
    return true;
  }

  /** Безпечний для показу перелік — без будь-яких ключів. */
  list() {
    return [...this.devices.entries()].map(([id, d]) => ({
      device_id: id,
      short: id.slice(0, 8),
      role: d.role,
      label: d.label,
      revoked: d.revoked === true,
      created_at: d.created_at,
      last_seen: d.last_seen,
      paired_count: d.paired_with.length,
    }));
  }

  get size() {
    return this.devices.size;
  }
}
