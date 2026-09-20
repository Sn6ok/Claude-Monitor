# Relay

> Relay маршрутизує повідомлення між ноутбуком і телефоном — і не бачить,
> що всередині. Він потрібен лише тому, що обидва пристрої зазвичай стоять
> за NAT і не можуть знайти один одного напряму.

---

## 1. Що робить і чого не робить

| Робить | Не робить |
|---|---|
| автентифікує пристрої | не дешифрує повідомлення |
| перевіряє право надсилати | не має доступу до файлів ноутбука |
| маршрутизує кадри | не бачить екрана |
| обмежує частоту | не зберігає подій |
| валідує вхід | не виконує AI-операцій |

Єдиний стан, який Relay зберігає між перезапусками, — реєстр пристроїв:
ролі, публічні ключі, звʼязки pairing, ознака відкликання. Подій він не
зберігає взагалі.

---

## 2. Швидкий старт

```powershell
cd relay
npm install
node src/index.js --dev
```

Режим `--dev` слухає `127.0.0.1:8787` без TLS. Він **дозволений лише на
локальному інтерфейсі** — спроба запустити його на зовнішній адресі
відхиляється конфігурацією.

Перевірка:

```powershell
curl http://127.0.0.1:8787/health
```

---

## 3. Розгортання через Cloudflare Tunnel

Рекомендований варіант: безкоштовно, з валідним сертифікатом, без білого IP
і без відкритих портів на роутері.

### Швидкий тунель

```powershell
winget install Cloudflare.cloudflared

# у першому вікні
cd relay
node src/index.js

# у другому вікні
cloudflared tunnel --url http://localhost:8787
```

Cloudflared надрукує адресу виду `https://abc-def-ghi.trycloudflare.com`.
Для застосунку вона потрібна як `wss://abc-def-ghi.trycloudflare.com/ws`.

**Обмеження:** за кожного запуску адреса нова. Для щоденного використання
краще іменований тунель.

### Іменований тунель зі сталою адресою

Потрібен домен, доданий у Cloudflare (безкоштовного тарифу вистачає).

```powershell
cloudflared tunnel login
cloudflared tunnel create claude-monitor
cloudflared tunnel route dns claude-monitor monitor.ваш-домен
```

Створіть `%USERPROFILE%\.cloudflared\config.yml`:

```yaml
tunnel: claude-monitor
credentials-file: C:\Users\<ви>\.cloudflared\<id>.json

ingress:
  - hostname: monitor.ваш-домен
    service: http://localhost:8787
  - service: http_status:404
```

Запуск:

```powershell
cloudflared tunnel run claude-monitor
```

Адреса для застосунку: `wss://monitor.ваш-домен/ws`.

Щоб тунель піднімався сам:

```powershell
cloudflared service install
```

### Чому саме так

Relay слухає `127.0.0.1` і **недосяжний з мережі напряму**. Тунель сам
відкриває вихідне зʼєднання до Cloudflare, тож на роутері нічого
налаштовувати не треба, а порт ноутбука лишається закритим.

---

## 4. Розгортання на власному сервері

Якщо Relay має працювати незалежно від ноутбука.

### З власним TLS

```bash
node src/index.js --mode tls --host 0.0.0.0 --port 443 \
  --cert /etc/letsencrypt/live/домен/fullchain.pem \
  --key  /etc/letsencrypt/live/домен/privkey.pem
```

### За зворотним проксі

Relay слухає локально, TLS термінує nginx:

```nginx
location /ws {
    proxy_pass http://127.0.0.1:8787;
    proxy_http_version 1.1;
    proxy_set_header Upgrade $http_upgrade;
    proxy_set_header Connection "upgrade";
    proxy_set_header X-Forwarded-For $remote_addr;

    # WebSocket живе довго: без цього nginx рватиме зʼєднання
    proxy_read_timeout 3600s;
}
```

### systemd

```ini
[Unit]
Description=Claude Monitor Relay
After=network.target

[Service]
Type=simple
User=claude-monitor
WorkingDirectory=/opt/claude-monitor/relay
ExecStart=/usr/bin/node src/index.js --mode tunnel --port 8787
Restart=on-failure
RestartSec=5

# Обмеження прав: Relay не потребує нічого зайвого
NoNewPrivileges=true
PrivateTmp=true
ProtectSystem=strict
ProtectHome=true
ReadWritePaths=/var/lib/claude-monitor

[Install]
WantedBy=multi-user.target
```

---

## 5. Режими

| Режим | Слухає | TLS | Коли використовувати |
|---|---|---|---|
| `tunnel` | лише локально | зовнішній | Cloudflare Tunnel, nginx (типовий) |
| `tls` | будь-де | власний | окремий сервер із сертифікатом |
| `plain` | лише локально | немає | розробка й тести |

`plain` на зовнішній адресі **відхиляється**: небезпечний варіант має бути
недосяжним випадково, а не лише не рекомендованим.

---

## 6. Налаштування

```text
--mode <tunnel|tls|plain>    режим (типово tunnel)
--host <адреса>              інтерфейс (типово 127.0.0.1)
--port <порт>                порт (типово 8787)
--data-dir <шлях>            каталог даних
--cert <шлях> --key <шлях>   сертифікат для режиму tls
--log-level <рівень>         error | warn | info | debug
--max-connections <n>        межа зʼєднань (типово 64)
```

Ті самі параметри доступні через змінні середовища з префіксом `CM_`,
наприклад `CM_PORT`, `CM_LOG_LEVEL`.

### Обмеження частоти

```text
--limit-conn-burst   30    сплеск нових зʼєднань з адреси
--limit-conn-rate    2     зʼєднань за секунду
--limit-auth-burst   5     спроб автентифікації
--limit-frame-burst  120   сплеск кадрів
--limit-frame-rate   40    кадрів за секунду
```

Ліміти винесені в конфігурацію свідомо: за одним NAT кілька пристроїв
мають спільну зовнішню адресу, і жорстко зашите значення блокувало б
легітимних користувачів.

Ліміт pairing (5 спроб, далі 15 хвилин) послаблювати не варто: саме він
робить перебір коду неможливим.

---

## 7. Керування пристроями

```powershell
node src/index.js devices              # перелік
node src/index.js revoke <device_id>   # відкликати доступ
```

Достатньо вказати початок ідентифікатора — повний вводити не треба.

Відкликання діє **негайно**: авторизація перевіряється на кожному кадрі,
а не лише при підключенні. Запис пристрою не видаляється, а позначається
відкликаним — так той самий ключ не зможе непомітно пройти pairing заново.

---

## 8. Що в журналі

```text
2026-09-09T16:55:22Z [info] [auth] успіх bridge:94dd677a
2026-09-09T16:55:33Z [debug] [pair] заявку передано 1 пропозиціям
2026-09-09T16:55:45Z [info] [conn] відключився monitor:1713875d
```

Записуються лише короткі ідентифікатори, коди подій і лічильники.
**Ніколи не записуються:** ключі, підписи, шифротекст, коди pairing,
вміст повідомлень. IP-адреси маскуються до `127.0.x.x`.

У робочому режимі рівень `info`; `debug` вмикається явно.

---

## 9. Ресурси

| | |
|---|---|
| RAM | ~49 МБ (базове споживання Node.js) |
| CPU у спокої | 0.000% |
| Залежності | одна: `ws`, без транзитивних |
| Дисковий запис | лише реєстр пристроїв, при змінах |

Relay розрахований на кілька пристроїв однієї людини, а не на багатьох
користувачів. Межа в 64 одночасні зʼєднання для цього надлишкова.

---

## 10. Health-ендпоінт

```json
GET /health  →  {"ok":true,"protocol":1,"uptime_sec":3820}
```

Навмисно не розкриває нічого про пристрої чи сесії. Усі інші маршрути
повертають 404.
