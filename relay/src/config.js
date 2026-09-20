/**
 * Конфігурація Relay.
 *
 * Принцип: безпечні значення за замовчуванням (Частина 3 §23 Master Prompt).
 * Жодного секрету в коді, жодного «якщо не вийшло — працюємо відкрито».
 */

import { homedir } from 'node:os';
import { join, resolve } from 'node:path';
import { existsSync, readFileSync } from 'node:fs';

const DEFAULT_DIR = join(homedir(), '.claude-monitor', 'relay');

/**
 * Режим прослуховування.
 *
 * `tunnel`  — Relay слухає HTTP на 127.0.0.1, а TLS назовні забезпечує
 *             Cloudflare Tunnel. Порт недоступний з мережі взагалі: тунель
 *             ініціює вихідне зʼєднання. Це режим за замовчуванням.
 *
 * `tls`     — Relay сам термінує TLS. Потрібні cert і key. Для розгортання
 *             на власному VPS із сертифікатом Let's Encrypt.
 *
 * `plain`   — HTTP на 127.0.0.1 без тунелю. ЛИШЕ для локальних тестів;
 *             блокується, якщо адреса не є локальною.
 */
export const MODES = Object.freeze(['tunnel', 'tls', 'plain']);

function parseArgs(argv) {
  const args = {};
  for (let i = 0; i < argv.length; i += 1) {
    const token = argv[i];
    if (!token.startsWith('--')) continue;
    const eq = token.indexOf('=');
    if (eq !== -1) {
      args[token.slice(2, eq)] = token.slice(eq + 1);
    } else {
      const next = argv[i + 1];
      if (next && !next.startsWith('--')) {
        args[token.slice(2)] = next;
        i += 1;
      } else {
        args[token.slice(2)] = 'true';
      }
    }
  }
  return args;
}

function asInt(value, fallback) {
  const parsed = Number.parseInt(value, 10);
  return Number.isFinite(parsed) ? parsed : fallback;
}

function asBool(value, fallback) {
  if (value === undefined) return fallback;
  return value === 'true' || value === '1' || value === true;
}

export function loadConfig(argv = process.argv.slice(2), env = process.env) {
  const args = parseArgs(argv);
  const dataDir = resolve(args['data-dir'] ?? env.CM_DATA_DIR ?? DEFAULT_DIR);

  const mode = args.mode ?? env.CM_MODE ?? (asBool(args.dev, false) ? 'plain' : 'tunnel');
  if (!MODES.includes(mode)) {
    throw new Error(`невідомий режим "${mode}", очікується один із: ${MODES.join(', ')}`);
  }

  const host = args.host ?? env.CM_HOST ?? '127.0.0.1';
  const port = asInt(args.port ?? env.CM_PORT, 8787);

  // Відкритий HTTP дозволений лише на локальному інтерфейсі. Це не можна
  // обійти прапорцем: небезпечний fallback заборонений Частиною 3 §23.
  const isLocal = host === '127.0.0.1' || host === 'localhost' || host === '::1';
  if (mode === 'plain' && !isLocal) {
    throw new Error(
      `режим "plain" дозволений лише на локальному інтерфейсі, отримано host=${host}. ` +
        'Для доступу з мережі використовуйте mode=tls або mode=tunnel.',
    );
  }
  if (mode === 'tunnel' && !isLocal) {
    throw new Error(
      `режим "tunnel" передбачає, що Relay слухає локально, а TLS забезпечує тунель. ` +
        `Отримано host=${host}. Для прямого прослуховування в мережі використовуйте mode=tls.`,
    );
  }

  const config = {
    mode,
    host,
    port,
    dataDir,
    devicesFile: join(dataDir, 'devices.json'),
    logLevel: args['log-level'] ?? env.CM_LOG_LEVEL ?? 'info',
    maxConnections: asInt(args['max-connections'] ?? env.CM_MAX_CONNECTIONS, 64),
    // Порожній список означає «дозволити будь-який Origin». Для WebSocket
    // це прийнятно, бо автентифікація не залежить від cookie й Origin.
    allowedOrigins: (args['allowed-origins'] ?? env.CM_ALLOWED_ORIGINS ?? '')
      .split(',')
      .map((s) => s.trim())
      .filter(Boolean),
    tls: null,

    /**
     * Профілі обмеження частоти. Винесені в конфігурацію свідомо: за одним
     * NAT (домашній роутер, мобільний оператор) кілька пристроїв мають спільну
     * зовнішню адресу, тож жорстко зашите значення блокувало б легітимних
     * користувачів. Значення за замовчуванням лишаються консервативними.
     */
    limits: {
      connection: {
        capacity: asInt(args['limit-conn-burst'] ?? env.CM_LIMIT_CONN_BURST, 30),
        refillPerSec: Number(args['limit-conn-rate'] ?? env.CM_LIMIT_CONN_RATE ?? 2),
      },
      auth: {
        capacity: asInt(args['limit-auth-burst'] ?? env.CM_LIMIT_AUTH_BURST, 5),
        refillPerSec: Number(args['limit-auth-rate'] ?? env.CM_LIMIT_AUTH_RATE ?? 0.1),
      },
      // Найжорсткіший ліміт. Послаблювати його не варто: саме він
      // унеможливлює перебір коду pairing (docs/protocol.md §4.2).
      pairing: {
        capacity: asInt(args['limit-pair-burst'] ?? env.CM_LIMIT_PAIR_BURST, 5),
        refillPerSec: Number(args['limit-pair-rate'] ?? env.CM_LIMIT_PAIR_RATE ?? 0.005),
      },
      frames: {
        capacity: asInt(args['limit-frame-burst'] ?? env.CM_LIMIT_FRAME_BURST, 120),
        refillPerSec: Number(args['limit-frame-rate'] ?? env.CM_LIMIT_FRAME_RATE ?? 40),
      },
    },
  };

  if (mode === 'tls') {
    const certPath = args.cert ?? env.CM_TLS_CERT;
    const keyPath = args.key ?? env.CM_TLS_KEY;
    if (!certPath || !keyPath) {
      throw new Error('режим "tls" потребує --cert і --key');
    }
    if (!existsSync(certPath)) throw new Error(`сертифікат не знайдено: ${certPath}`);
    if (!existsSync(keyPath)) throw new Error(`приватний ключ не знайдено: ${keyPath}`);

    config.tls = {
      cert: readFileSync(certPath),
      key: readFileSync(keyPath),
      // TLS 1.2 — нижня межа. Старіші версії мають відомі вади
      // і заборонені Частиною 3 §2.
      minVersion: 'TLSv1.2',
    };
  }

  return config;
}

/** Публічний опис конфігурації для логів — без сертифікатів і ключів. */
export function describeConfig(config) {
  return {
    mode: config.mode,
    listen: `${config.host}:${config.port}`,
    dataDir: config.dataDir,
    maxConnections: config.maxConnections,
    tls: config.mode === 'tls' ? 'увімкнено (власний сертифікат)' : 'зовнішній термінатор',
  };
}
