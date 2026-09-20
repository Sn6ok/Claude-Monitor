import { WebSocket } from "ws";
const url = process.argv[2];
// Без спеціальних заголовків — так само, як робитиме застосунок.
const ws = new WebSocket(url);
const timer = setTimeout(() => { console.log("ТАЙМАУТ"); process.exit(1); }, 25000);
ws.on("open", () => {
  console.log("WebSocket ВІДКРИВСЯ");
  ws.send(JSON.stringify({ v:1, t:"ping", id:"0".repeat(26), ts:Date.now(), p:{} }));
});
ws.on("message", (d) => {
  console.log("ВІДПОВІДЬ RELAY:", d.toString().slice(0,100));
  clearTimeout(timer); ws.close(); process.exit(0);
});
ws.on("error", (e) => { console.log("ПОМИЛКА:", e.message); clearTimeout(timer); process.exit(1); });
