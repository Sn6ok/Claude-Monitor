/**
 * Secure Relay Server.
 *
 * Відповідальність (Частина 6 §7 Master Prompt):
 *   автентифікація -> авторизація -> маршрутизація -> валідація -> ліміти
 *
 * Чого Relay не робить ніколи: не запускає Claude, не має доступу до файлової
 * системи ноутбука, не бачить екрана, не зберігає подій, не дешифрує вміст.
 * Корисне навантаження проходить крізь нього як непрозорий шифротекст.
 */

import { createServer as createHttpServer } from 'node:http';
import { createServer as createHttpsServer } from 'node:https';
import { WebSocketServer } from 'ws';

import { parseFrame, makeFrame, makeError, LIMITS, PROTOCOL_VERSION } from './protocol.js';
import {
  verifyAuthSignature,
  deviceIdFromPublicKey,
  makeChallengeNonce,
  randomBytes,
} from './crypto.js';
import { DeviceRegistry } from './devices.js';
import { RateLimiter, PROFILES, PAIRING_LOCKOUT_SEC, AUTH_LOCKOUT_SEC } from './ratelimit.js';

/** Стани життєвого циклу зʼєднання. Кадр не за станом — відхиляється. */
const STATE = Object.freeze({
  NEW: 'new',
  CHALLENGED: 'challenged',
  AUTHENTICATED: 'authenticated',
  CLOSING: 'closing',
});

const CHALLENGE_TTL_MS = 30_000;
const HANDSHAKE_TIMEOUT_MS = 15_000;
const PAIRING_TTL_MS = 180_000;
const PING_INTERVAL_MS = 45_000;
const PONG_GRACE_MS = 20_000;
const MAX_PAIRING_OFFERS = 8;

const LOG_LEVELS = { error: 0, warn: 1, info: 2, debug: 3 };

export class RelayServer {
  constructor(config, logger = console) {
    this.config = config;
    this.rawLogger = logger;
    this.levelValue = LOG_LEVELS[config.logLevel] ?? LOG_LEVELS.info;

    this.registry = new DeviceRegistry(config.devicesFile, this);

    // Конфігурація може перевизначити профілі; за її відсутності
    // застосовуються консервативні значення за замовчуванням.
    const profiles = config.limits ?? {};
    this.limits = {
      connection: new RateLimiter(profiles.connection ?? PROFILES.connection),
      auth: new RateLimiter(profiles.auth ?? PROFILES.auth),
      pairing: new RateLimiter(profiles.pairing ?? PROFILES.pairing),
      frames: new RateLimiter(profiles.frames ?? PROFILES.frames),
    };

    /** @type {Map<string, object>} deviceId -> активне зʼєднання */
    this.connections = new Map();
    /** @type {Set<object>} усі сокети, зокрема ще не автентифіковані */
    this.sockets = new Set();
    /** @type {Map<string, object>} offerId -> пропозиція pairing (лише в памʼяті) */
    this.pairingOffers = new Map();

    this.httpServer = null;
    this.wss = null;
    this.timers = [];

    this.stats = {
      startedAt: Date.now(),
      connectionsTotal: 0,
      authOk: 0,
      authFailed: 0,
      framesRouted: 0,
      framesRejected: 0,
      pairingsCompleted: 0,
    };
  }

  // ── Логування ────────────────────────────────────────────────────────────
  //
  // Логи мають бути достатніми для діагностики й не містити секретів
  // (Частина 2 §17, Частина 3 §24). Тому назовні йдуть лише короткі
  // ідентифікатори, коди й лічильники — ніколи ключі, підписи чи шифротекст.

  log(level, message) {
    if ((LOG_LEVELS[level] ?? 9) > this.levelValue) return;
    const line = `${new Date().toISOString()} [${level}] ${message}`;
    if (level === 'error') this.rawLogger.error(line);
    else if (level === 'warn') this.rawLogger.warn(line);
    else this.rawLogger.log(line);
  }

  error(m) { this.log('error', m); }
  warn(m) { this.log('warn', m); }
  info(m) { this.log('info', m); }
  debug(m) { this.log('debug', m); }

  // ── Запуск і зупинка ─────────────────────────────────────────────────────

  async start() {
    const requestHandler = (req, res) => this.handleHttpRequest(req, res);

    this.httpServer = this.config.tls
      ? createHttpsServer(this.config.tls, requestHandler)
      : createHttpServer(requestHandler);

    this.wss = new WebSocketServer({
      server: this.httpServer,
      path: '/ws',
      maxPayload: LIMITS.MAX_FRAME_BYTES,
      // Стиснення вимкнено свідомо: воно дало б економію лише на великих
      // повідомленнях, яких тут немає, натомість коштувало б CPU та памʼяті
      // на обох кінцях і додало б клас атак на стиснення шифротексту.
      perMessageDeflate: false,
      verifyClient: (info, done) => this.verifyClient(info, done),
    });

    this.wss.on('connection', (socket, req) => this.handleConnection(socket, req));
    this.wss.on('error', (err) => this.error(`[wss] ${err.message}`));

    await new Promise((resolve, reject) => {
      this.httpServer.once('error', reject);
      this.httpServer.listen(this.config.port, this.config.host, () => {
        this.httpServer.removeListener('error', reject);
        resolve();
      });
    });

    this.timers.push(setInterval(() => this.sweep(), 10_000));
    this.timers.forEach((t) => t.unref?.());

    this.info(
      `relay слухає ${this.config.host}:${this.config.port} ` +
        `(режим ${this.config.mode}, протокол v${PROTOCOL_VERSION}, ` +
        `пристроїв у реєстрі: ${this.registry.size})`,
    );
  }

  async stop() {
    this.timers.forEach(clearInterval);
    this.timers = [];
    for (const socket of this.sockets) {
      try { socket.close(1001, 'server shutdown'); } catch { /* сокет уже мертвий */ }
    }
    this.sockets.clear();
    this.connections.clear();
    this.pairingOffers.clear();

    await new Promise((resolve) => {
      if (!this.wss) return resolve();
      this.wss.close(() => resolve());
    });
    await new Promise((resolve) => {
      if (!this.httpServer) return resolve();
      this.httpServer.close(() => resolve());
    });
    this.info('relay зупинено');
  }

  // ── HTTP ─────────────────────────────────────────────────────────────────

  /**
   * Relay не є вебсервером. Єдиний доступний HTTP-маршрут — health-перевірка
   * без будь-яких даних про пристрої чи сесії.
   */
  handleHttpRequest(req, res) {
    if (req.method === 'GET' && req.url === '/health') {
      const body = JSON.stringify({
        ok: true,
        protocol: PROTOCOL_VERSION,
        uptime_sec: Math.floor((Date.now() - this.stats.startedAt) / 1000),
      });
      res.writeHead(200, {
        'content-type': 'application/json',
        'cache-control': 'no-store',
        'x-content-type-options': 'nosniff',
      });
      res.end(body);
      return;
    }
    res.writeHead(404, { 'content-type': 'text/plain', 'x-content-type-options': 'nosniff' });
    res.end('not found');
  }

  verifyClient(info, done) {
    const nowMs = Date.now();
    const ip = this.clientIp(info.req);

    const { allowed, retryAfterSec } = this.limits.connection.check(ip, nowMs);
    if (!allowed) {
      this.warn(`[conn] відхилено за лімітом: ${this.maskIp(ip)}, повтор через ${retryAfterSec}с`);
      return done(false, 429, 'Too Many Requests');
    }

    if (this.sockets.size >= this.config.maxConnections) {
      this.warn(`[conn] досягнуто межі зʼєднань (${this.config.maxConnections})`);
      return done(false, 503, 'Service Unavailable');
    }

    const origins = this.config.allowedOrigins;
    if (origins.length > 0) {
      const origin = info.origin ?? info.req.headers.origin;
      if (origin && !origins.includes(origin)) {
        this.warn('[conn] відхилено за Origin');
        return done(false, 403, 'Forbidden');
      }
    }

    return done(true);
  }

  clientIp(req) {
    // За тунелем або зворотним проксі справжня адреса приходить у заголовку.
    // Довіряти йому можна лише тому, що Relay у цих режимах слухає локально
    // і недосяжний напряму (див. config.js).
    if (this.config.mode !== 'tls') {
      const forwarded = req.headers['cf-connecting-ip'] ?? req.headers['x-forwarded-for'];
      if (typeof forwarded === 'string' && forwarded.length > 0) {
        return forwarded.split(',')[0].trim().slice(0, 64);
      }
    }
    return req.socket.remoteAddress ?? 'unknown';
  }

  /** У логах адреса маскується: діагностики вистачає, приватності більше. */
  maskIp(ip) {
    if (ip.includes(':')) return `${ip.split(':').slice(0, 2).join(':')}:…`;
    const parts = ip.split('.');
    return parts.length === 4 ? `${parts[0]}.${parts[1]}.x.x` : 'unknown';
  }

  // ── Життєвий цикл зʼєднання ──────────────────────────────────────────────

  handleConnection(socket, req) {
    const nowMs = Date.now();
    const conn = {
      socket,
      ip: this.clientIp(req),
      state: STATE.NEW,
      role: null,
      deviceId: null,
      nonce: null,
      nonceExpiresAt: 0,
      connectedAt: nowMs,
      lastSeenAt: nowMs,
      lastPongAt: nowMs,
      framesIn: 0,
    };

    this.sockets.add(socket);
    socket._cm = conn;
    this.stats.connectionsTotal += 1;
    this.debug(`[conn] нове зʼєднання з ${this.maskIp(conn.ip)}`);

    // Зʼєднання, яке не пройшло рукостискання за відведений час, закривається.
    // Інакше сокети, відкриті й покинуті, накопичувались би без обмежень.
    //
    // Виняток — pairing. Таке зʼєднання НЕ може бути автентифікованим за
    // визначенням: пристрій саме зараз реєструється і ще не має ключів
    // у реєстрі. Bridge при цьому чекає на телефон усі 180 секунд життя
    // коду, тож 15-секундний таймаут обривав його на середині й повертав
    // «auth_failed» ще до того, як користувач устигав ввести код.
    conn.handshakeTimer = setTimeout(() => {
      if (conn.state === STATE.AUTHENTICATED) return;
      if (conn.pendingOfferId) return;  // pairing триває — не чіпаємо

      this.debug('[conn] таймаут рукостискання');
      this.closeWith(conn, 'auth_failed', 4401);
    }, HANDSHAKE_TIMEOUT_MS);
    conn.handshakeTimer.unref?.();

    // Окремий запобіжник для pairing: зʼєднання живе не довше за саму
    // пропозицію плюс невеликий запас. Це не дає покинутим сокетам
    // накопичуватися, але й не обриває процедуру на середині.
    conn.pairingTimer = setTimeout(() => {
      if (conn.state === STATE.AUTHENTICATED) return;
      if (!conn.pendingOfferId) return;

      this.debug('[conn] час pairing вичерпано');
      try { socket.close(1000, 'pairing expired'); } catch { /* уже закрито */ }
    }, PAIRING_TTL_MS + 15_000);
    conn.pairingTimer.unref?.();

    socket.on('message', (data, isBinary) => this.handleMessage(conn, data, isBinary));
    socket.on('pong', () => { conn.lastPongAt = Date.now(); });
    socket.on('close', () => this.handleClose(conn));
    socket.on('error', (err) => {
      this.debug(`[conn] помилка сокета: ${err.message}`);
      this.handleClose(conn);
    });
  }

  handleClose(conn) {
    if (conn.handshakeTimer) clearTimeout(conn.handshakeTimer);
    if (conn.pairingTimer) clearTimeout(conn.pairingTimer);
    this.sockets.delete(conn.socket);

    if (conn.deviceId && this.connections.get(conn.deviceId) === conn) {
      this.connections.delete(conn.deviceId);
      this.info(`[conn] відключився ${conn.role}:${conn.deviceId.slice(0, 8)}`);
      this.notifyPeersPresence(conn.deviceId, false);
    }
    conn.state = STATE.CLOSING;
  }

  send(conn, payload) {
    if (conn.socket.readyState !== conn.socket.OPEN) return false;
    try {
      conn.socket.send(payload);
      return true;
    } catch (err) {
      this.debug(`[send] не вдалося: ${err.message}`);
      return false;
    }
  }

  closeWith(conn, code, wsCode = 1008, retryAfterSec) {
    this.send(conn, makeError(code, randomBytes, retryAfterSec));
    conn.state = STATE.CLOSING;
    // Невелика затримка дає кадру помилки дійти до клієнта раніше,
    // ніж закриється сокет — інакше клієнт побачить лише обрив.
    setTimeout(() => {
      try { conn.socket.close(wsCode, code); } catch { /* уже закрито */ }
    }, 50).unref?.();
  }

  // ── Обробка кадрів ───────────────────────────────────────────────────────

  handleMessage(conn, data, isBinary) {
    if (conn.state === STATE.CLOSING) return;

    const nowMs = Date.now();
    conn.lastSeenAt = nowMs;
    conn.framesIn += 1;

    // Ліміт частоти кадрів. До автентифікації ключем є IP, після —
    // ідентифікатор пристрою: інакше один клієнт за NAT міг би вичерпати
    // ліміт для всіх інших.
    const limitKey = conn.deviceId ?? conn.ip;
    const rate = this.limits.frames.check(limitKey, nowMs);
    if (!rate.allowed) {
      this.stats.framesRejected += 1;
      this.warn(`[rate] перевищено частоту кадрів: ${this.maskIp(conn.ip)}`);
      this.closeWith(conn, 'rate_limited', 1013, rate.retryAfterSec);
      return;
    }

    const parsed = parseFrame(data, isBinary, nowMs);
    if (!parsed.ok) {
      this.stats.framesRejected += 1;
      this.debug(`[frame] відхилено: ${parsed.code}/${parsed.detail ?? '-'}`);
      this.closeWith(conn, parsed.code, parsed.code === 'too_large' ? 1009 : 1007);
      return;
    }

    const frame = parsed.frame;

    // Кадр має відповідати стану зʼєднання. Це прибирає цілий клас атак,
    // де клієнт намагається надіслати fwd до автентифікації.
    switch (frame.t) {
      case 'hello':      return this.onHello(conn, frame, nowMs);
      case 'auth':       return this.onAuth(conn, frame, nowMs);
      case 'ping':       return void this.send(conn, makeFrame('pong', {}, randomBytes, nowMs));
      case 'pong':       return;
      case 'fwd':        return this.onForward(conn, frame, nowMs);
      case 'pair_offer': return this.onPairOffer(conn, frame, nowMs);
      case 'pair_claim': return this.onPairClaim(conn, frame, nowMs);
      case 'pair_ok':    return this.onPairOk(conn, frame, nowMs);
      case 'pair_cancel':return this.onPairCancel(conn, frame);
      default:
        this.closeWith(conn, 'bad_frame', 1007);
    }
  }

  onHello(conn, frame, nowMs) {
    if (conn.state !== STATE.NEW) return this.closeWith(conn, 'bad_frame', 1002);

    const { role, device_id: deviceId } = frame.p;
    conn.role = role;
    conn.deviceId = deviceId;
    conn.state = STATE.CHALLENGED;
    conn.nonce = makeChallengeNonce();
    conn.nonceExpiresAt = nowMs + CHALLENGE_TTL_MS;

    this.debug(`[hello] ${role}:${deviceId.slice(0, 8)}`);

    this.send(
      conn,
      makeFrame('challenge', { nonce: conn.nonce, expires_at: conn.nonceExpiresAt }, randomBytes, nowMs),
    );
  }

  onAuth(conn, frame, nowMs) {
    if (conn.state !== STATE.CHALLENGED) return this.closeWith(conn, 'bad_frame', 1002);

    const authKey = `${conn.ip}|${frame.p.device_id}`;
    const rate = this.limits.auth.check(authKey, nowMs);
    if (!rate.allowed) {
      this.warn(`[auth] ліміт спроб вичерпано: ${conn.deviceId?.slice(0, 8)}`);
      return this.closeWith(conn, 'rate_limited', 1013, rate.retryAfterSec);
    }

    const reject = (reason) => {
      this.stats.authFailed += 1;
      this.limits.auth.penalize(authKey, nowMs, AUTH_LOCKOUT_SEC);
      this.warn(`[auth] невдача (${reason}): ${frame.p.device_id.slice(0, 8)}`);
      // Клієнту повертається однаковий код незалежно від причини —
      // щоб не давати підказок про те, які device_id зареєстровані.
      this.closeWith(conn, 'auth_failed', 4401);
    };

    // device_id у hello та auth мають збігатися.
    if (frame.p.device_id !== conn.deviceId) return reject('невідповідність device_id');

    // Nonce одноразовий і короткоживучий (Частина 3 §10).
    if (!conn.nonce || nowMs > conn.nonceExpiresAt) return reject('nonce застарів');

    const device = this.registry.get(conn.deviceId);
    if (!device) return reject('пристрій невідомий');
    if (device.revoked === true) {
      this.stats.authFailed += 1;
      this.warn(`[auth] відкликаний пристрій: ${conn.deviceId.slice(0, 8)}`);
      // Тут код інший навмисно: застосунок має стерти локальні ключі
      // й показати екран pairing, а не повторювати спроби.
      return this.closeWith(conn, 'device_revoked', 4403);
    }
    if (device.role !== conn.role) return reject('невідповідність ролі');

    const valid = verifyAuthSignature({
      role: conn.role,
      deviceId: conn.deviceId,
      nonceB64Url: conn.nonce,
      signatureB64Url: frame.p.sig,
      publicKeyRawB64Url: device.pub_sig,
    });
    if (!valid) return reject('підпис не пройшов перевірку');

    // Nonce згорає одразу після використання — повторне подання
    // того самого кадру вже не пройде (replay protection).
    conn.nonce = null;

    // Одне активне зʼєднання на пристрій. Старе витісняється: інакше
    // після зміни мережі накопичувалися б «привидні» сокети.
    const previous = this.connections.get(conn.deviceId);
    if (previous && previous !== conn) {
      this.debug(`[auth] витісняю попереднє зʼєднання ${conn.deviceId.slice(0, 8)}`);
      try { previous.socket.close(4409, 'replaced'); } catch { /* уже мертве */ }
    }

    conn.state = STATE.AUTHENTICATED;
    this.connections.set(conn.deviceId, conn);
    if (conn.handshakeTimer) clearTimeout(conn.handshakeTimer);
    if (conn.pairingTimer) clearTimeout(conn.pairingTimer);

    device.last_seen = nowMs;
    this.stats.authOk += 1;

    const peerRole = conn.role === 'bridge' ? 'monitor' : 'bridge';
    const peers = this.registry.peersOf(conn.deviceId, peerRole);
    const onlinePeers = peers.filter((id) => this.connections.has(id));
    const peerOnline = onlinePeers.length > 0;

    this.info(`[auth] успіх ${conn.role}:${conn.deviceId.slice(0, 8)}`);

    const authPayload = {
      session_token: randomBytes(16).toString('base64url'),
      peer_online: peerOnline,
      server_time: nowMs,
    };
    // Bridge обслуговує кілька телефонів: йому потрібно знати, які саме
    // з них у мережі, — кожен отримує власний знімок стану.
    if (conn.role === 'bridge') authPayload.peers_online = onlinePeers;

    this.send(conn, makeFrame('auth_ok', authPayload, randomBytes, nowMs));

    this.notifyPeersPresence(conn.deviceId, true);
  }

  /**
   * Повідомляє партнерів про появу чи зникнення пристрою.
   * Це дозволяє Android одразу показати «ноутбук офлайн», не чекаючи таймауту,
   * а Bridge за device_id дізнається, котрий із його телефонів з'явився.
   */
  notifyPeersPresence(deviceId, online) {
    const device = this.registry.get(deviceId);
    if (!device) return;

    for (const peerId of device.paired_with) {
      const peerConn = this.connections.get(peerId);
      if (!peerConn || peerConn.state !== STATE.AUTHENTICATED) continue;
      this.send(
        peerConn,
        makeFrame('error', { code: online ? 'peer_online' : 'peer_offline', device_id: deviceId }, randomBytes),
      );
    }
  }

  /**
   * Маршрутизація. Тут Relay виконує свою головну роботу — і при цьому
   * не заглядає в `ct`. Він перевіряє лише право надсилати й пересилає байти.
   *
   * Bridge може обслуговувати до п'яти телефонів і шифрує кожному окремим
   * ключем, тому вказує адресата в `to`: кадр іде лише йому. Без `to` кадр
   * іде всім партнерам, як і раніше. Отримувач бачить `from` — за ним Bridge
   * обирає ключ, яким розшифрувати кадр від телефона.
   */
  onForward(conn, frame, nowMs) {
    if (conn.state !== STATE.AUTHENTICATED) return this.closeWith(conn, 'bad_frame', 1002);

    const peerRole = conn.role === 'bridge' ? 'monitor' : 'bridge';
    const peers = this.registry.peersOf(conn.deviceId, peerRole);

    if (peers.length === 0) {
      this.send(conn, makeError('not_paired', randomBytes));
      return;
    }

    const target = typeof frame.p.to === 'string' ? frame.p.to : null;
    const recipients = target ? peers.filter((id) => id === target) : peers;

    let delivered = 0;
    for (const peerId of recipients) {
      // Авторизація перевіряється на кожному кадрі, а не лише при вході:
      // відкликання має діяти негайно, а не з наступного підключення.
      if (!this.registry.isAuthorizedPair(conn.deviceId, peerId)) continue;

      const peerConn = this.connections.get(peerId);
      if (!peerConn || peerConn.state !== STATE.AUTHENTICATED) continue;

      // Кадр перезбирається сервером: назовні йде лише перевірене
      // корисне навантаження, без будь-яких зайвих полів від клієнта.
      const outgoing = makeFrame(
        'fwd',
        { n: frame.p.n, ct: frame.p.ct, from: conn.deviceId },
        randomBytes,
        nowMs,
      );
      if (this.send(peerConn, outgoing)) delivered += 1;
    }

    if (delivered === 0) {
      const payload = { code: 'peer_offline' };
      if (target) payload.device_id = target;
      this.send(conn, makeFrame('error', payload, randomBytes));
    } else {
      this.stats.framesRouted += delivered;
    }
  }

  // ── Pairing ──────────────────────────────────────────────────────────────
  //
  // Relay бере участь у pairing, але не може його підробити: коди
  // підтвердження обчислені на секреті, якого сервер не бачить
  // (docs/protocol.md §4).

  onPairOffer(conn, frame, nowMs) {
    // Пропозицію робить Bridge. Він ще не зареєстрований, тож приймається
    // до автентифікації — але з найжорсткішим лімітом частоти.
    if (conn.role !== null && conn.role !== 'bridge') {
      return this.closeWith(conn, 'bad_frame', 1002);
    }

    const rate = this.limits.pairing.check(conn.ip, nowMs);
    if (!rate.allowed) {
      this.warn(`[pair] ліміт пропозицій: ${this.maskIp(conn.ip)}`);
      return this.closeWith(conn, 'rate_limited', 1013, rate.retryAfterSec);
    }

    if (this.pairingOffers.size >= MAX_PAIRING_OFFERS) {
      this.expirePairingOffers(nowMs);
      if (this.pairingOffers.size >= MAX_PAIRING_OFFERS) {
        return void this.send(conn, makeError('rate_limited', randomBytes, 60));
      }
    }

    const { offer_id: offerId, pub_sig: pubSig, pub_ecdh: pubEcdh, confirm } = frame.p;

    const derivedId = deviceIdFromPublicKey(pubSig);
    if (!derivedId) return void this.send(conn, makeError('bad_frame', randomBytes));

    this.pairingOffers.set(offerId, {
      offerId,
      bridgeConn: conn,
      bridgeDeviceId: derivedId,
      bridgePubSig: pubSig,
      bridgePubEcdh: pubEcdh,
      bridgeConfirm: confirm,
      createdAt: nowMs,
      expiresAt: nowMs + PAIRING_TTL_MS,
      claimed: false,
    });

    conn.pendingOfferId = offerId;
    this.info(`[pair] пропозицію зареєстровано ${offerId.slice(0, 8)} (діє 180 с)`);
  }

  onPairClaim(conn, frame, nowMs) {
    if (conn.role !== null && conn.role !== 'monitor') {
      return this.closeWith(conn, 'bad_frame', 1002);
    }

    const rate = this.limits.pairing.check(conn.ip, nowMs);
    if (!rate.allowed) {
      this.warn(`[pair] ліміт спроб: ${this.maskIp(conn.ip)}`);
      return this.closeWith(conn, 'rate_limited', 1013, rate.retryAfterSec);
    }

    this.expirePairingOffers(nowMs);

    // Порожній offer_id означає «не знаю, якій саме пропозиції» — так і буває,
    // бо користувач вводить лише код. Тоді заявка йде всім активним
    // пропозиціям, а підтвердить її лише той Bridge, у якого збігся код.
    const targets = frame.p.offer_id
      ? [this.pairingOffers.get(frame.p.offer_id)].filter(Boolean)
      : [...this.pairingOffers.values()];

    const usable = targets.filter((offer) => !offer.claimed && offer.expiresAt >= nowMs);

    // Відсутність придатних пропозицій дає ту саму відповідь, що й хибний
    // код: інакше перебором можна було б з'ясувати, чи triває pairing.
    if (usable.length === 0) {
      this.limits.pairing.penalize(conn.ip, nowMs, PAIRING_LOCKOUT_SEC);
      this.warn(`[pair] невдала спроба з ${this.maskIp(conn.ip)}`);
      return void this.send(conn, makeError('auth_failed', randomBytes));
    }

    const monitorDeviceId = deviceIdFromPublicKey(frame.p.pub_sig);
    if (!monitorDeviceId) return void this.send(conn, makeError('bad_frame', randomBytes));

    // Позначаємо зʼєднання монітора як таке, що проходить pairing.
    // Без цього спрацьовує загальний таймаут рукостискання і Relay
    // обриває телефон за 15 секунд — рівно перед тим, як Bridge надішле
    // підтвердження. Ззовні це виглядало як «час очікування вичерпано»
    // на телефоні при успішному pairing на ноутбуці.
    conn.pendingOfferId = frame.p.offer_id || usable[0].offerId;

    let delivered = 0;
    for (const offer of usable) {
      if (offer.bridgeConn.socket.readyState !== offer.bridgeConn.socket.OPEN) continue;

      // Заявка позначається як передана саме цьому Bridge. Якщо код не
      // збігся, Bridge відповість відмовою, і пропозиція звільниться.
      offer.monitorConn = conn;
      offer.monitorDeviceId = monitorDeviceId;
      offer.monitorPubSig = frame.p.pub_sig;
      offer.monitorPubEcdh = frame.p.pub_ecdh;

      this.send(
        offer.bridgeConn,
        makeFrame(
          'pair_claim',
          {
            offer_id: offer.offerId,
            pub_sig: frame.p.pub_sig,
            pub_ecdh: frame.p.pub_ecdh,
            confirm: frame.p.confirm,
          },
          randomBytes,
          nowMs,
        ),
      );
      delivered += 1;
    }

    if (delivered === 0) {
      this.send(conn, makeError('peer_offline', randomBytes));
    } else {
      this.debug(`[pair] заявку передано ${delivered} пропозиціям`);
    }
  }

  /** Рішення Bridge: код збігся чи ні. Тільки після цього зʼявляється запис. */
  onPairOk(conn, frame, nowMs) {
    const offer = this.pairingOffers.get(frame.p.offer_id);
    if (!offer || offer.bridgeConn !== conn) return;

    if (!frame.p.accepted) {
      // Код не збігся саме з цією пропозицією. Пропозиція лишається
      // активною до кінця свого часу життя: заявку могли розіслати
      // кільком Bridge, або користувач просто помилився і введе ще раз.
      this.warn(`[pair] Bridge відхилив заявку ${offer.offerId.slice(0, 8)}`);
      const monitorConn = offer.monitorConn;
      offer.monitorConn = null;
      offer.monitorDeviceId = null;
      if (monitorConn) this.send(monitorConn, makeError('auth_failed', randomBytes));
      return;
    }

    // Успіх: пропозиція одноразова й більше не діє.
    this.pairingOffers.delete(offer.offerId);

    const bridgeUpsert = this.registry.upsert(
      { deviceId: offer.bridgeDeviceId, role: 'bridge', pubSig: offer.bridgePubSig, pubEcdh: offer.bridgePubEcdh, label: 'Windows Bridge' },
      nowMs,
    );
    const monitorUpsert = this.registry.upsert(
      { deviceId: offer.monitorDeviceId, role: 'monitor', pubSig: offer.monitorPubSig, pubEcdh: offer.monitorPubEcdh, label: 'Android Monitor' },
      nowMs,
    );

    if (!bridgeUpsert.ok || !monitorUpsert.ok) {
      // Той самий device_id уже існує з іншими ключами — це або помилка,
      // або спроба підміни. Реєстр не чіпаємо.
      this.error('[pair] відмова: device_id уже зареєстровано з іншим ключем');
      if (offer.monitorConn) this.send(offer.monitorConn, makeError('auth_failed', randomBytes));
      this.send(conn, makeError('auth_failed', randomBytes));
      return;
    }

    this.registry.pair(offer.bridgeDeviceId, offer.monitorDeviceId);
    this.stats.pairingsCompleted += 1;
    this.info(
      `[pair] успішно: bridge:${offer.bridgeDeviceId.slice(0, 8)} <-> monitor:${offer.monitorDeviceId.slice(0, 8)}`,
    );

    const okFrame = makeFrame(
      'pair_ok',
      { offer_id: offer.offerId, accepted: true, peer_pub_sig: offer.bridgePubSig, peer_pub_ecdh: offer.bridgePubEcdh },
      randomBytes,
      nowMs,
    );

    // Факт доставки підтвердження логуємо явно: без цього неможливо
    // відрізнити «телефон не отримав» від «телефон отримав і не обробив».
    if (!offer.monitorConn) {
      this.warn('[pair] підтвердження нікуди надіслати: зʼєднання телефона відсутнє');
      return;
    }

    const socketState = offer.monitorConn.socket.readyState;
    const delivered = this.send(offer.monitorConn, okFrame);

    if (delivered) {
      this.info(`[pair] підтвердження надіслано телефону (${okFrame.length} байтів)`);
    } else {
      this.warn(`[pair] НЕ вдалося надіслати підтвердження, стан сокета=${socketState}`);
    }
  }

  onPairCancel(conn, frame) {
    const offer = this.pairingOffers.get(frame.p.offer_id);
    if (offer && (offer.bridgeConn === conn || offer.monitorConn === conn)) {
      this.pairingOffers.delete(frame.p.offer_id);
      this.debug(`[pair] пропозицію скасовано ${frame.p.offer_id.slice(0, 8)}`);
    }
  }

  expirePairingOffers(nowMs) {
    for (const [id, offer] of this.pairingOffers) {
      if (offer.expiresAt < nowMs) {
        this.pairingOffers.delete(id);
        this.debug(`[pair] пропозиція протермінована ${id.slice(0, 8)}`);
      }
    }
  }

  // ── Періодичне обслуговування ────────────────────────────────────────────

  /**
   * Прибирання мертвих зʼєднань і протермінованих пропозицій.
   *
   * Це єдиний періодичний таймер сервера. Інтервал 10 с обрано так, щоб
   * навантаження було непомітним, але «зависле» зʼєднання не трималося
   * хвилинами.
   */
  sweep() {
    const nowMs = Date.now();
    this.expirePairingOffers(nowMs);

    for (const socket of this.sockets) {
      const conn = socket._cm;
      if (!conn) continue;

      if (conn.state === STATE.AUTHENTICATED) {
        const silentMs = nowMs - conn.lastSeenAt;
        if (silentMs > PING_INTERVAL_MS) {
          if (nowMs - conn.lastPongAt > PING_INTERVAL_MS + PONG_GRACE_MS) {
            this.debug(`[sweep] немає відповіді ${conn.deviceId?.slice(0, 8)}, закриваю`);
            try { socket.terminate(); } catch { /* уже мертве */ }
            continue;
          }
          try { socket.ping(); } catch { /* уже мертве */ }
        }
      }
    }
  }

  /** Метрики без жодних персональних даних — для діагностики. */
  getStats() {
    return {
      ...this.stats,
      uptime_sec: Math.floor((Date.now() - this.stats.startedAt) / 1000),
      sockets: this.sockets.size,
      authenticated: this.connections.size,
      pending_pairings: this.pairingOffers.size,
      devices: this.registry.size,
    };
  }
}
