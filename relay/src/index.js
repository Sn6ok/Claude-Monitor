#!/usr/bin/env node
/**
 * Точка входу Relay.
 *
 * Крім запуску сервера, надає команди керування пристроями — зокрема
 * відкликання, яке потрібне за сценарієм втраченого телефона
 * (Частина 3 §21, §22 Master Prompt).
 *
 *   node src/index.js                      запустити сервер
 *   node src/index.js --dev                локальний режим для розробки
 *   node src/index.js devices              перелік пристроїв
 *   node src/index.js revoke <device_id>   відкликати пристрій
 */

import { execSync } from 'node:child_process';

import { loadConfig, describeConfig } from './config.js';
import { DeviceRegistry } from './devices.js';
import { RelayServer } from './server.js';

/**
 * Консоль Windows за замовчуванням використовує однобайтову кодову сторінку
 * (для української локалі — 1251), тому UTF-8 із Node виводиться кракозябрами.
 * Перемикання на 65001 робить український текст читабельним.
 *
 * Помилка тут не критична: у гіршому разі текст буде негарним, але робота
 * сервера від цього не залежить.
 */
function enableUtf8Console() {
  if (process.platform !== 'win32') return;
  try {
    execSync('chcp 65001', { stdio: 'ignore' });
  } catch {
    /* немає chcp або перенаправлений вивід — не біда */
  }
}

const HELP = `
Claude Monitor Relay

Використання:
  node src/index.js [опції]              запустити сервер
  node src/index.js devices              показати зареєстровані пристрої
  node src/index.js revoke <device_id>   відкликати доступ пристрою
  node src/index.js --help               ця довідка

Опції:
  --mode <tunnel|tls|plain>   режим роботи (типово tunnel)
  --host <адреса>             інтерфейс (типово 127.0.0.1)
  --port <порт>               порт (типово 8787)
  --data-dir <шлях>           каталог даних (типово ~/.claude-monitor/relay)
  --cert <шлях> --key <шлях>  сертифікат і ключ для режиму tls
  --log-level <рівень>        error | warn | info | debug
  --dev                       локальний режим (mode=plain на 127.0.0.1)

Режими:
  tunnel  Relay слухає локально, TLS назовні дає Cloudflare Tunnel.
          Порт недоступний із мережі. Рекомендований варіант.
  tls     Relay сам термінує TLS. Для власного VPS із сертифікатом.
  plain   HTTP лише на локальному інтерфейсі. Тільки для тестів.
`;

function commandDevices(config) {
  const registry = new DeviceRegistry(config.devicesFile);
  const devices = registry.list();

  if (devices.length === 0) {
    console.log('Зареєстрованих пристроїв немає.');
    console.log(`Реєстр: ${config.devicesFile}`);
    return 0;
  }

  console.log(`Зареєстровані пристрої (${devices.length}):\n`);
  for (const d of devices) {
    const status = d.revoked ? 'ВІДКЛИКАНО' : 'активний';
    const seen = d.last_seen ? new Date(d.last_seen).toLocaleString('uk-UA') : 'ніколи';
    console.log(`  ${d.short}…  ${d.role.padEnd(8)}  ${status.padEnd(10)}  ${d.label}`);
    console.log(`    повний id: ${d.device_id}`);
    console.log(`    парних пристроїв: ${d.paired_count}   останній вхід: ${seen}\n`);
  }
  return 0;
}

function commandRevoke(config, deviceIdArg) {
  if (!deviceIdArg) {
    console.error('Потрібно вказати device_id. Перелік: node src/index.js devices');
    return 2;
  }

  const registry = new DeviceRegistry(config.devicesFile);

  // Дозволяємо скорочений префікс — повний id незручно вводити руками.
  const matches = registry.list().filter((d) => d.device_id.startsWith(deviceIdArg.toLowerCase()));

  if (matches.length === 0) {
    console.error(`Пристрій "${deviceIdArg}" не знайдено.`);
    return 1;
  }
  if (matches.length > 1) {
    console.error(`Префікс "${deviceIdArg}" неоднозначний, підходить ${matches.length} пристроїв.`);
    console.error('Вкажіть довший префікс або повний device_id.');
    return 1;
  }

  const target = matches[0];
  if (target.revoked) {
    console.log(`Пристрій ${target.short}… (${target.label}) вже відкликано.`);
    return 0;
  }

  registry.revoke(target.device_id);
  console.log(`Пристрій ${target.short}… (${target.label}) відкликано.`);
  console.log('Наявні зʼєднання буде розірвано, старі ключі більше не дійсні.');
  console.log('Для повторного доступу потрібно пройти pairing заново.');
  return 0;
}

async function commandServe(config) {
  const server = new RelayServer(config);
  const described = describeConfig(config);

  console.log('Claude Monitor Relay');
  for (const [key, value] of Object.entries(described)) {
    console.log(`  ${key.padEnd(16)} ${value}`);
  }
  console.log('');

  if (config.mode === 'plain') {
    console.warn('УВАГА: режим "plain" без TLS. Допустимий лише для локальних тестів.');
    console.warn('Для доступу з телефона через інтернет використовуйте tunnel або tls.\n');
  }

  await server.start();

  let stopping = false;
  const shutdown = async (signal) => {
    if (stopping) return;
    stopping = true;
    console.log(`\nОтримано ${signal}, завершую роботу…`);
    await server.stop();
    process.exit(0);
  };

  process.on('SIGINT', () => void shutdown('SIGINT'));
  process.on('SIGTERM', () => void shutdown('SIGTERM'));

  // Непередбачена помилка не повинна залишати сервер у невизначеному стані:
  // краще коректно зупинитись, ніж продовжувати з пошкодженим станом.
  process.on('uncaughtException', (err) => {
    console.error(`Критична помилка: ${err.message}`);
    console.error(err.stack);
    void shutdown('uncaughtException');
  });
  process.on('unhandledRejection', (reason) => {
    console.error(`Необроблена відмова промісу: ${reason}`);
  });

  return new Promise(() => {}); // працюємо, доки не прийде сигнал
}

async function main() {
  enableUtf8Console();
  const argv = process.argv.slice(2);

  if (argv.includes('--help') || argv.includes('-h')) {
    console.log(HELP.trim());
    return 0;
  }

  const command = argv[0] && !argv[0].startsWith('--') ? argv[0] : 'serve';
  const rest = command === 'serve' ? argv : argv.slice(1);

  let config;
  try {
    config = loadConfig(rest);
  } catch (err) {
    console.error(`Помилка конфігурації: ${err.message}`);
    return 2;
  }

  switch (command) {
    case 'serve':   return commandServe(config);
    case 'devices': return commandDevices(config);
    case 'revoke':  return commandRevoke(config, rest[0]);
    default:
      console.error(`Невідома команда "${command}".`);
      console.error(HELP.trim());
      return 2;
  }
}

main().then(
  (code) => { if (typeof code === 'number' && code !== 0) process.exit(code); },
  (err) => {
    console.error(`Не вдалося запуститись: ${err.message}`);
    process.exit(1);
  },
);
