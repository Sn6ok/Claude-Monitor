/**
 * Обмеження частоти запитів (Частина 3 §20 Master Prompt).
 *
 * Реалізовано алгоритмом «дірявого відра» (token bucket): він дозволяє
 * короткі сплески, але тримає середню швидкість у межах. На відміну від
 * лічильника у фіксованому вікні, тут немає стрибка на межі вікна.
 *
 * Пам'ять обмежена явно: коли записів стає більше за ліміт, найдавніші
 * неактивні витісняються. Це прибирає можливість вичерпати RAM сервера
 * потоком запитів з різних адрес.
 */

export class TokenBucket {
  /**
   * @param {number} capacity     максимальний запас токенів (розмір сплеску)
   * @param {number} refillPerSec швидкість поповнення, токенів за секунду
   */
  constructor(capacity, refillPerSec) {
    this.capacity = capacity;
    this.refillPerSec = refillPerSec;
    this.tokens = capacity;
    this.lastRefillMs = 0;
    this.blockedUntilMs = 0;
  }

  /** @returns {{allowed: boolean, retryAfterSec: number}} */
  take(nowMs, cost = 1) {
    if (nowMs < this.blockedUntilMs) {
      return { allowed: false, retryAfterSec: Math.ceil((this.blockedUntilMs - nowMs) / 1000) };
    }

    if (this.lastRefillMs === 0) {
      this.lastRefillMs = nowMs;
    } else if (nowMs > this.lastRefillMs) {
      const elapsedSec = (nowMs - this.lastRefillMs) / 1000;
      this.tokens = Math.min(this.capacity, this.tokens + elapsedSec * this.refillPerSec);
      this.lastRefillMs = nowMs;
    }

    if (this.tokens >= cost) {
      this.tokens -= cost;
      return { allowed: true, retryAfterSec: 0 };
    }

    const deficit = cost - this.tokens;
    return { allowed: false, retryAfterSec: Math.max(1, Math.ceil(deficit / this.refillPerSec)) };
  }

  /** Жорстке блокування — застосовується після серії невдалих спроб. */
  blockFor(nowMs, seconds) {
    this.blockedUntilMs = Math.max(this.blockedUntilMs, nowMs + seconds * 1000);
    this.tokens = 0;
  }

  isIdle(nowMs, idleMs) {
    return nowMs - this.lastRefillMs > idleMs && nowMs >= this.blockedUntilMs;
  }
}

export class RateLimiter {
  /**
   * @param {object}  opts
   * @param {number}  opts.capacity
   * @param {number}  opts.refillPerSec
   * @param {number}  opts.maxKeys      верхня межа кількості записів у пам'яті
   * @param {number}  opts.idleMs       після цього простою запис можна витіснити
   */
  constructor({ capacity, refillPerSec, maxKeys = 10_000, idleMs = 600_000 }) {
    this.capacity = capacity;
    this.refillPerSec = refillPerSec;
    this.maxKeys = maxKeys;
    this.idleMs = idleMs;
    /** @type {Map<string, TokenBucket>} */
    this.buckets = new Map();
  }

  bucketFor(key, nowMs) {
    let bucket = this.buckets.get(key);
    if (!bucket) {
      if (this.buckets.size >= this.maxKeys) this.evictIdle(nowMs);
      bucket = new TokenBucket(this.capacity, this.refillPerSec);
      this.buckets.set(key, bucket);
    }
    return bucket;
  }

  check(key, nowMs = Date.now(), cost = 1) {
    return this.bucketFor(key, nowMs).take(nowMs, cost);
  }

  penalize(key, nowMs, seconds) {
    this.bucketFor(key, nowMs).blockFor(nowMs, seconds);
  }

  /**
   * Витісняє неактивні записи. Якщо неактивних не знайшлося — видаляє
   * найстаріші, бо необмежене зростання Map є гіршою проблемою,
   * ніж скидання лічильника для кількох клієнтів.
   */
  evictIdle(nowMs) {
    const doomed = [];
    for (const [key, bucket] of this.buckets) {
      if (bucket.isIdle(nowMs, this.idleMs)) doomed.push(key);
    }
    if (doomed.length === 0) {
      const excess = Math.max(1, Math.floor(this.maxKeys * 0.1));
      let i = 0;
      for (const key of this.buckets.keys()) {
        if (i >= excess) break;
        doomed.push(key);
        i += 1;
      }
    }
    for (const key of doomed) this.buckets.delete(key);
  }

  get size() {
    return this.buckets.size;
  }
}

/**
 * Профілі лімітів.
 *
 * Значення підібрані так, щоб нормальна робота ніколи їх не досягала:
 * Bridge надсилає події пачками не частіше кількох разів на секунду,
 * а pairing відбувається лічені рази за весь час життя пристрою.
 */
export const PROFILES = Object.freeze({
  /** Нові TCP-з'єднання з однієї IP-адреси. */
  connection: { capacity: 10, refillPerSec: 0.5 },

  /** Спроби автентифікації. Невдача додатково карається блокуванням. */
  auth: { capacity: 5, refillPerSec: 0.1 },

  /**
   * Спроби pairing. Найжорсткіший ліміт: 5 спроб, далі 15 хвилин блокування
   * (docs/protocol.md §4.2). Це унеможливлює перебір коду навіть теоретично.
   */
  pairing: { capacity: 5, refillPerSec: 0.005 },

  /** Кадри маршрутизації від автентифікованого пристрою. */
  frames: { capacity: 120, refillPerSec: 40 },
});

export const PAIRING_LOCKOUT_SEC = 900;
export const AUTH_LOCKOUT_SEC = 60;
