// NCSC Smart Water - WhatsApp bridge server
// ESP32  --->  this server  --->  Twilio  --->  your WhatsApp
// You    --->  WhatsApp  --->  Twilio  --->  this server  --->  ESP32 (polls)

const express = require("express");
const twilio = require("twilio");

const {
  TWILIO_ACCOUNT_SID,
  TWILIO_AUTH_TOKEN,
  TWILIO_WHATSAPP_FROM,   // e.g. whatsapp:+14155238886 (Twilio sandbox number)
  OWNER_WHATSAPP,         // e.g. whatsapp:+91XXXXXXXXXX (YOUR number)
  ESP32_TOKEN,            // same secret as API_TOKEN in the ESP32 code
  PUBLIC_URL,             // e.g. https://your-app.onrender.com
  PORT = 3000,
} = process.env;

const client = twilio(TWILIO_ACCOUNT_SID, TWILIO_AUTH_TOKEN);
const app = express();

app.use(express.json());
app.use(express.urlencoded({ extended: false })); // Twilio sends form data

// ---------------- state ----------------
let pendingCommand = null;      // "START" or null
let pendingCommandTime = 0;
const COMMAND_EXPIRY_MS = 2 * 60 * 1000; // stale START commands are dropped

let lastReport = null;          // last alert/status from the ESP32
let lastSeen = 0;               // last time ESP32 contacted the server

// ---------------- helpers ----------------
function sendWhatsApp(text) {
  return client.messages.create({
    from: TWILIO_WHATSAPP_FROM,
    to: OWNER_WHATSAPP,
    body: text,
  });
}

function requireEsp32(req, res, next) {
  if (req.get("Authorization") !== `Bearer ${ESP32_TOKEN}`) {
    return res.status(401).json({ error: "unauthorised" });
  }
  lastSeen = Date.now();
  next();
}

// ---------------- ESP32 -> server ----------------

// Shutdown / leak alert
app.post("/api/leak-alert", requireEsp32, async (req, res) => {
  const d = req.body;
  lastReport = { ...d, time: new Date().toISOString() };

  const text =
    `🚨 *WATER LEAK - PUMP STOPPED*\n` +
    `Location: ${d.section}\n` +
    `Inlet (FS1): ${d.flow1} L/min\n` +
    `Middle (FS2): ${d.flow2} L/min\n` +
    `Outlet (FS3): ${d.flow3} L/min\n` +
    `Section 1 loss: ${d.section1LossRate} L/min\n` +
    `Section 2 loss: ${d.section2LossRate} L/min\n` +
    `Total water lost: ${d.waterLoss} L\n` +
    `Soil moisture: ${d.soil1}%\n` +
    (d.gpsFix
      ? `Map: https://maps.google.com/?q=${d.latitude},${d.longitude}\n`
      : `GPS: no fix\n`) +
    `\nFix the leak, then reply *START* to restart the pump.`;

  try {
    await sendWhatsApp(text);
    res.json({ ok: true });
  } catch (e) {
    console.error("Twilio error:", e.message);
    res.status(502).json({ error: "whatsapp send failed" }); // ESP32 will retry
  }
});

// Pump started confirmation
app.post("/api/pump-started", requireEsp32, async (req, res) => {
  try {
    await sendWhatsApp(`✅ Pump STARTED (source: ${req.body.source || "unknown"}).`);
    res.json({ ok: true });
  } catch (e) {
    console.error("Twilio error:", e.message);
    res.status(502).json({ error: "whatsapp send failed" });
  }
});

// ESP32 polls this to see if you sent START
app.get("/api/command", requireEsp32, (req, res) => {
  if (pendingCommand && Date.now() - pendingCommandTime > COMMAND_EXPIRY_MS) {
    pendingCommand = null;
  }
  const cmd = pendingCommand || "NONE";
  pendingCommand = null; // deliver once
  res.json({ command: cmd });
});

// ---------------- WhatsApp -> server (Twilio webhook) ----------------
app.post("/whatsapp", (req, res) => {
  const valid = twilio.validateRequest(
    TWILIO_AUTH_TOKEN,
    req.get("X-Twilio-Signature") || "",
    `${PUBLIC_URL}/whatsapp`,
    req.body
  );
  if (!valid) return res.status(403).send("Forbidden");

  const reply = new twilio.twiml.MessagingResponse();

  // Only YOUR number may control the pump
  if (req.body.From !== OWNER_WHATSAPP) {
    return res.status(403).send("Forbidden");
  }

  const msg = (req.body.Body || "").trim().toUpperCase();

  if (msg === "START") {
    pendingCommand = "START";
    pendingCommandTime = Date.now();
    reply.message("Start command received. The pump will start within a few seconds if it is stopped.");
  } else if (msg === "STATUS") {
    const online = Date.now() - lastSeen < 30000;
    reply.message(
      `System: ${online ? "ONLINE" : "OFFLINE"}\n` +
      (lastReport
        ? `Last leak report: ${lastReport.section} at ${lastReport.time}`
        : "No leak reports yet.")
    );
  } else {
    reply.message("Commands:\nSTART - restart the pump\nSTATUS - system status");
  }

  res.type("text/xml").send(reply.toString());
});

app.get("/", (req, res) => res.send("NCSC water server running"));

app.listen(PORT, () => console.log(`Server listening on ${PORT}`));
