#include "net.h"
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <esp_sntp.h>
#include <sys/time.h>
#include "hw.h"

#define CONNECT_TIMEOUT_MS 20000
#define SYNC_TIMEOUT_MS 15000
#define SETUP_TIMEOUT_MS (10 * 60 * 1000UL)
#define RETRY_MS (10 * 60 * 1000UL)       // after a failed attempt
#define RESYNC_MS (6 * 3600 * 1000UL)     // after a good sync
#define DEFAULT_TZ "CET-1CEST,M3.5.0,M10.5.0/3"

static Preferences prefs;
static String ssid, pass, tz;
static NetState state = NET_OFF;
static uint32_t stateSince = 0, nextAttempt = 0;
static bool synced = false;
static const char *message = "";
static WebServer server(80);
static DNSServer dns;
static bool serverStarted = false;
static uint32_t connectAfterSave = 0;
static volatile bool keep = false;  // stay online, e.g. for the pomodoro radio

static void enter(NetState s) {
  state = s;
  stateSince = millis();
}

static void wifiOff() {
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
}

static void applyTz() {
  setenv("TZ", tz.c_str(), 1);
  tzset();
}

static void startConnect() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid.c_str(), pass.c_str());
  message = "Подключаюсь...";
  enter(NET_CONNECTING);
}

// ---- Setup page ----

static String escape(const String &s) {
  String o;
  for (char c : s) {
    if (c == '<') o += "&lt;";
    else if (c == '>') o += "&gt;";
    else if (c == '"') o += "&quot;";
    else if (c == '&') o += "&amp;";
    else o += c;
  }
  return o;
}

static const char PAGE_HEAD[] PROGMEM = R"(<!doctype html><html lang="ru"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1"><title>Тамагочи: Wi-Fi</title>
<style>body{font-family:system-ui,sans-serif;max-width:420px;margin:24px auto;padding:0 16px;background:#faf7f0;color:#241a1a}
h1{font-size:22px}label{display:block;margin:14px 0 4px;font-weight:600}
select,input{width:100%;box-sizing:border-box;font-size:17px;padding:10px;border:2px solid #241a1a;border-radius:8px;background:#fff}
button{margin-top:20px;width:100%;font-size:18px;padding:12px;border:0;border-radius:8px;background:#ec964a;color:#241a1a;font-weight:700}
.note{color:#776;font-size:14px}</style></head><body><h1>&#x1F436; Тамагочи: Wi-Fi</h1>)";

// The phone knows its time zone by name; the badge needs it as a POSIX rule.
static const char PAGE_TZ_SCRIPT[] PROGMEM = R"(<script>
var cet="CET-1CEST,M3.5.0,M10.5.0/3",eet="EET-2EEST,M3.5.0/3,M10.5.0/4",z={
"Europe/London":"GMT0BST,M3.5.0/1,M10.5.0","Europe/Dublin":"GMT0IST,M3.5.0/1,M10.5.0","Europe/Lisbon":"WET0WEST,M3.5.0/1,M10.5.0",
"Europe/Moscow":"MSK-3","Europe/Minsk":"<+03>-3","Europe/Istanbul":"<+03>-3","Asia/Tbilisi":"<+04>-4","Asia/Yerevan":"<+04>-4",
"Asia/Baku":"<+04>-4","Asia/Dubai":"<+04>-4","Asia/Almaty":"<+05>-5","Asia/Tashkent":"<+05>-5","Asia/Jerusalem":"IST-2IDT,M3.4.4/26,M10.5.0",
"America/New_York":"EST5EDT,M3.2.0,M11.1.0","America/Chicago":"CST6CDT,M3.2.0,M11.1.0","America/Los_Angeles":"PST8PDT,M3.2.0,M11.1.0"};
["Berlin","Vienna","Zurich","Amsterdam","Brussels","Paris","Madrid","Rome","Prague","Warsaw","Budapest","Copenhagen","Stockholm","Oslo",
"Belgrade","Zagreb","Ljubljana","Bratislava","Luxembourg","Malta","Monaco","Podgorica","Sarajevo","Skopje","Tirane"].forEach(function(c){z["Europe/"+c]=cet});
["Kyiv","Kiev","Helsinki","Riga","Tallinn","Vilnius","Sofia","Bucharest","Athens"].forEach(function(c){z["Europe/"+c]=eet});
var n=Intl.DateTimeFormat().resolvedOptions().timeZone,t=z[n];
if(!t){var o=-new Date().getTimezoneOffset(),a=Math.abs(o),h=Math.floor(a/60),m=a%60,s=o<0?"-":"+";
t="<"+s+(h<10?"0":"")+h+(m?m:"")+">"+(o<0?"":"-")+h+(m?":"+m:"");}
document.getElementById("tz").value=t;document.getElementById("tzname").textContent=n||t;
</script></body></html>)";

static void handleRoot() {
  String page = FPSTR(PAGE_HEAD);
  int n = WiFi.scanComplete();
  if (n == WIFI_SCAN_RUNNING) {
    page += F("<p>Ищу сети...</p><script>setTimeout(function(){location.reload()},2000)</script></body></html>");
    server.send(200, "text/html; charset=utf-8", page);
    return;
  }
  page += F("<form method='post' action='/save'><label for='ssid'>Сеть</label><select id='ssid' name='ssid'>");
  for (int i = 0; i < n; i++) {
    String name = WiFi.SSID(i);
    if (!name.length() || page.indexOf("value=\"" + escape(name) + "\"") >= 0) continue;
    page += "<option value=\"" + escape(name) + "\"" + (name == ssid ? " selected" : "") + ">" + escape(name) +
            "</option>";
  }
  page += F("</select><label for='other'>Или введи имя сети</label><input id='other' name='other' autocomplete='off'>"
            "<label for='pass'>Пароль</label><input id='pass' name='pass' type='password'>"
            "<input type='hidden' id='tz' name='tz'><p class='note'>Часовой пояс: <span id='tzname'></span></p>"
            "<button type='submit'>Сохранить</button></form>"
            "<p class='note'>Бейдж выходит в интернет только чтобы узнать время.</p>");
  page += FPSTR(PAGE_TZ_SCRIPT);
  server.send(200, "text/html; charset=utf-8", page);
}

static void handleSave() {
  String other = server.arg("other");
  ssid = other.length() ? other : server.arg("ssid");
  pass = server.arg("pass");
  if (server.arg("tz").length()) tz = server.arg("tz");
  prefs.putString("ssid", ssid);
  prefs.putString("pass", pass);
  prefs.putString("tz", tz);
  applyTz();
  String page = FPSTR(PAGE_HEAD);
  page += "<p>Сохранено. Бейдж подключается к «" + escape(ssid) +
          "», это займёт несколько секунд. Можно закрыть эту страницу.</p></body></html>";
  server.send(200, "text/html; charset=utf-8", page);
  connectAfterSave = millis() + 1500;  // let the page reach the phone first
}

static void handleRedirect() {
  server.sendHeader("Location", String("http://") + SETUP_AP_IP + "/", true);
  server.send(302, "text/plain", "");
}

void netStartSetup() {
  WiFi.disconnect(true);
  WiFi.mode(WIFI_AP_STA);
  bool up = WiFi.softAP(SETUP_AP_NAME);
  Serial.printf("setup network %s: %s\n", SETUP_AP_NAME, up ? WiFi.softAPIP().toString().c_str() : "failed");
  WiFi.scanNetworks(true);
  dns.start(53, "*", WiFi.softAPIP());
  if (!serverStarted) {
    server.on("/", HTTP_GET, handleRoot);
    server.on("/save", HTTP_POST, handleSave);
    server.onNotFound(handleRedirect);  // phones probe a few URLs to detect the setup page
    serverStarted = true;
  }
  server.begin();
  message = "";
  enter(NET_SETUP);
}

void netStopSetup() {
  if (state != NET_SETUP) return;
  dns.stop();
  server.stop();
  WiFi.softAPdisconnect(true);
  wifiOff();
  enter(NET_OFF);
  nextAttempt = millis() + RETRY_MS;
}

// ---- Life cycle ----

void netBegin() {
  prefs.begin("net", false);
  ssid = prefs.getString("ssid", "");
  pass = prefs.getString("pass", "");
  tz = prefs.getString("tz", DEFAULT_TZ);
  applyTz();
  uint32_t t = rtcNow();
  if (t) {
    struct timeval tv = {(time_t)t + (time_t)UNIX_2000, 0};
    settimeofday(&tv, nullptr);
  }
  wifiOff();
  nextAttempt = millis() + 3000;  // let the screen come up first
}

void netLoop() {
  uint32_t now = millis();
  switch (state) {
    case NET_SETUP:
      dns.processNextRequest();
      server.handleClient();
      if (connectAfterSave && (int32_t)(now - connectAfterSave) >= 0) {
        connectAfterSave = 0;
        dns.stop();
        server.stop();
        WiFi.softAPdisconnect(true);
        startConnect();
      } else if (now - stateSince > SETUP_TIMEOUT_MS) {
        netStopSetup();
      }
      break;

    case NET_CONNECTING:
      if (WiFi.status() == WL_CONNECTED) {
        sntp_set_sync_status(SNTP_SYNC_STATUS_RESET);
        configTzTime(tz.c_str(), "pool.ntp.org", "time.google.com");
        message = "Узнаю время...";
        enter(NET_SYNCING);
      } else if (now - stateSince > CONNECT_TIMEOUT_MS) {
        message = "Не получилось подключиться";
        wifiOff();
        enter(NET_OFF);
        nextAttempt = now + (keep ? 15000 : RETRY_MS);
      }
      break;

    case NET_SYNCING:
      if (sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED && time(nullptr) > 1767225600) {  // after 2026-01-01
        rtcSet((uint32_t)(time(nullptr) - UNIX_2000));
        synced = true;
        message = "Готово";
        nextAttempt = now + RESYNC_MS;
        if (keep) {
          enter(NET_ONLINE);
        } else {
          wifiOff();
          enter(NET_OFF);
        }
      } else if (now - stateSince > SYNC_TIMEOUT_MS) {
        message = "Нет ответа от сервера времени";
        nextAttempt = now + RETRY_MS;
        if (keep) {
          enter(NET_ONLINE);
        } else {
          wifiOff();
          enter(NET_OFF);
        }
      }
      break;

    case NET_ONLINE:
      if (!keep) {
        wifiOff();
        enter(NET_OFF);
      } else if (WiFi.status() != WL_CONNECTED && now - stateSince > 5000) {
        startConnect();  // dropped: join again
      }
      break;

    case NET_OFF:
      if (ssid.length() && (keep || (int32_t)(now - nextAttempt) >= 0)) startConnect();
      break;
  }
}

void netKeepOnline(bool on) { keep = on; }
bool netOnline() { return (state == NET_SYNCING || state == NET_ONLINE) && WiFi.status() == WL_CONNECTED; }

NetState netState() { return state; }
bool netConfigured() { return ssid.length() > 0; }
bool netTimeSynced() { return synced; }
const char *netMessage() { return message; }
