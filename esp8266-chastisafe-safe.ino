// Using an ESP8266 as a replacement controller for a digital safe board
// providing a web-UI for control
//
// Specifically configured for chastisafe API
//
// (c) 2024 bdsm@spuddy.org
//
// MIT License

#include <ESP8266WiFi.h>
#include <ESP8266mDNS.h>
#include <EEPROM.h>
#include <ESP8266WebServer.h>
#include <ESP8266HTTPClient.h>
#include <ArduinoOTA.h>
#include <FS.h>
#include <LittleFS.h>
#define FSTYPE LittleFS

#define ARDUINOJSON_USE_LONG_LONG 1
#include <ArduinoJson.h>

#include "html.h"
#include "version.h"
#include "certs.h"

// This is pin D6 on most boards; this is the pin that needs to be
// connected to the relay
#define default_pin D6

// We need to store some values in EEPROM.
//   safe UI username
//   safe UI password
//   WiFi SSID
//   WiFi password
//   Access token
//   Lock ID
//   API URL
//   Safe Name
//   Pin to open solenoid
//
// For simplicitly we'll limit them to 100 characters each and sprinkle
// them through the EEPROM at 128 byte offsets
// It's not very efficient, but we have up to 4096 bytes to play with
// so...
//
// We put a magic string in front to detect if the values are good or not

#define EEPROM_SIZE 2048
#define maxpwlen 100
#define eeprom_magic F("PSWD:")
#define eeprom_magic_len 5

#define ui_username_offset   0
#define ui_pswd_offset       128
#define ui_wifi_ssid_offset  256
#define ui_wifi_pswd_offset  384
#define token_offset         512
#define lockid_offset        896
#define apiurl_offset        1024
#define safename_offset      1152
#define pin_offset           1280

enum safestate
{
  UNLOCKED,
  LOCKED
};


/////////////////////////////////////////

// Global variables
int state=UNLOCKED;
boolean wifi_connected;
boolean allow_updates = false;

// For communication to the API
BearSSL::WiFiClientSecure client;
BearSSL::CertStore certStore;
StaticJsonDocument<10000> raw_data;

// If we want insecure communication via http
// WiFiClient client;

#define DEFAULT_API F("https://chastisafe.com/api-v1/")

// These can be read at startup time from EEPROM
String ui_username;
String ui_pswd;
String wifi_ssid;
String wifi_pswd;
String token;
String lockid;
String apiurl;
String safename;
String pinstr;
int    pin;

// Create the webserver structure for port 80
ESP8266WebServer server(80);

/////////////////////////////////////////

// Read/write EEPROM values

String get_pswd(int offset)
{ 
  char d[maxpwlen];
  String pswd;

  for (int i=0; i<maxpwlen; i++)
  { 
    d[i]=EEPROM.read(offset+i);
  }

  pswd=String(d);
  
  if (pswd.startsWith(eeprom_magic))
  { 
    pswd=pswd.substring(eeprom_magic_len);
  }
  else
  {
    pswd="";
  }

  return pswd;
}

void set_pswd(String s, int offset, bool commit=true)
{ 
  String pswd=eeprom_magic + s;

  for(int i=0; i < pswd.length(); i++)
  {
    EEPROM.write(offset+i, pswd[i]);
  }
  EEPROM.write(offset+pswd.length(), 0);
  if (commit)
    EEPROM.commit();
}

/////////////////////////////////////////

void send_text(String s)
{
  server.send(200, F("text/html"), s);
}

/////////////////////////////////////////

// Routine to talk to Chastisafe

bool good_api_result;
  
void talk_to_api(String endpoint, String prefix, JsonDocument filter)
{
  good_api_result = false;

  if (token == "")
    return send_text(prefix + F("Missing API token.  Please set this up"));

  Serial.println(F("Filter is:"));
  serializeJsonPretty(filter, Serial);
  Serial.println();

  HTTPClient https;
  https.useHTTP10(true);

  // Ensure we can see the cookie header
  const char * headerkeys[] = {"Set-Cookie"};
  size_t headerkeyssize = sizeof(headerkeys)/sizeof(char*);
  https.collectHeaders(headerkeys,headerkeyssize);

  Serial.println(apiurl + endpoint);
  if (https.begin(client, apiurl + endpoint))
  { 
    https.addHeader(F("Cookie"),F("chastisafe=") + token);

    // start connection and send HTTP header
    int httpCode = https.GET();

    if (httpCode != 200)
    {
      https.end();
      return send_text(prefix + F("Problems talking to API: Response ") + String(httpCode) + " " + https.errorToString(httpCode) + F("<br>For reference, 403 likely means your access token is wrong"));
    }

    // Do we have a cookie header?
    String c=https.header("Set-Cookie");
    int endofval=c.indexOf(";");
    if (c.startsWith("chastisafe=") && endofval != -1)
    {
      // Find the ; which is the end of the value
      token=c.substring(11,endofval);
      Serial.println(F("Cookie: ") + c);
      Serial.println(F("New token: ") + token);
      set_pswd(token, token_offset, true);
    }

    // Now we need to parse the JSON data
    DeserializationError error = deserializeJson(raw_data, https.getStream(), DeserializationOption::Filter(filter));

    if (error)
      return send_text(prefix + F("Unable to decode JSON! ") + String(error.c_str()));

    Serial.println(F("JSON is:"));
    serializeJsonPretty(raw_data, Serial);
    Serial.println();

    good_api_result=true;
    https.end();
  }
  else
    return send_text(prefix + F("Could not connect to server"));
}

void get_locks()
{
  StaticJsonDocument<400> filter;
  filter[0]["id"] = true;
  filter[0]["lockname"] = true;
  filter[0]["loadtime"] = true;
  filter[0]["keyholder"] = true;

  return talk_to_api("loaded","",filter);
}

void get_recent()
{
  StaticJsonDocument<400> filter;
  filter[0]["lockid"] = true;
  filter[0]["unlockreason"] = true;

  return talk_to_api("recent","",filter);
}

int find_lock(String id)
{
  int res=-1;
  int l=raw_data.size();
  for (int i=0;i<l;i++)
  {
    if (raw_data[i]["id"] == id || raw_data[i]["lockid"] == id)
      res=i;
  }
  return res;
}

String html_clean(String s)
{
  String n=s;
  n.replace(F("<"),F("&lt;"));
  return n;
}

void renew(bool quiet=false)
{
  StaticJsonDocument<400> filter;
  talk_to_api("renew","",filter);
  if (!quiet)
    send_text("Attempted to renew");
}

/////////////////////////////////////////

// These functions manipulate the safe state.
// They look like they don't take any params, because they use the
// global server state variable to read the request.  This only works
// 'cos the main loop is single threaded!

void do_unlock(String reason)
{
  lockid = "";
  set_pswd(lockid, lockid_offset);
  state=UNLOCKED;
  return send_text(reason);
}

void status(String prefix="")
{
  if (lockid == "")
    return send_text(prefix + F("No lock has been set up.  The safe may be opened."));

  get_locks();
  if (!good_api_result)
    return;

  int l=find_lock(lockid);
  if (l == -1) // Not a current running lock, check recent history
  {
    get_recent();

    if (!good_api_result)
      return;

    l = find_lock(lockid);
    if ( l == -1)  // If we can't find the lock in either list, panic!
      return send_text(prefix + F("Unable to find lock in current or recent locks; remaining locked"));

    // Unlock reason 2 means abandoned; we don't unlock for that!
    String r=raw_data[l]["unlockreason"];
    Serial.println(F("Safe end reason is ")+r);
    if (r == "2")
      return send_text(prefix + F("This lock was abandoned; the safe will not open"));

    return do_unlock(prefix + F("<p>The lock has competed, and has been removed from the safe<p>The safe may be opened."));
  }
  return send_text(prefix + F("Safe is locked"));
}

void opensafe()
{
  if (state == LOCKED)
  {
    send_text(F("Can not open; lock is still running"));
  }
  else
  {
    String d = server.arg("duration");
    int del=d.toInt();
    if (del==0) { del=5; }
    digitalWrite(LED_BUILTIN, LOW);
    digitalWrite(pin, HIGH);
    server.setContentLength(CONTENT_LENGTH_UNKNOWN);
    send_text(F("Unlocking safe for ") + String(del) + F(" seconds<br>"));
    while(del--)
    {
      delay(1000);
      server.sendContent(String(del) + F("...\n"));
      if (del%10 == 0) server.sendContent(F("<br>"));
    }
    digitalWrite(pin, LOW);
    digitalWrite(LED_BUILTIN, HIGH);
    server.sendContent(F("Completed"));
    server.sendContent("");
  }
}

void display_auth()
{
  String page = FPSTR(change_auth_html);
         page.replace(F("##token##"), token);
         page.replace(F("##apiurl##"), apiurl);
         page.replace(F("##ui_username##"), ui_username);

  send_text(page);
}

void set_ap()
{
  if (server.hasArg("setwifi"))
  {
     Serial.println(F("Setting WiFi client"));
     safename=server.arg("safename");
     safename.replace(F(".local"),"");
     if (safename != "")
     {
       Serial.println(F("  Setting mDNS name"));
       set_pswd(safename,safename_offset);
     }

     pinstr=server.arg("pin");
     if (pinstr != "")
     {
       Serial.println(F("  Setting active pin"));
       set_pswd(pinstr,pin_offset);
     }

     if (server.arg("ssid") != "" && server.arg("password") != "")
     {
       Serial.println(F("  Setting network"));
       set_pswd(server.arg("ssid"), ui_wifi_ssid_offset, false);
       set_pswd(server.arg("password"), ui_wifi_pswd_offset);
     }
     send_text(F("Restarting in 5 seconds"));
     delay(5000);
     ESP.restart();
  }
  String page = FPSTR(change_ap_html);
         page.replace(F("##safename##"), safename);
         page.replace(F("##pin##"), String(pin));
         page.replace(F("##VERSION##"), VERSION);
  send_text(page);
}

void set_auth()
{
  Serial.println(F("Setting Auth details"));
  ui_username=server.arg("username");
  if (server.arg("password") != "")
    ui_pswd=server.arg("password");

  set_pswd(ui_username, ui_username_offset,false);
  set_pswd(ui_pswd, ui_pswd_offset);

  send_text(F("Password reset"));
}

void set_api()
{
  Serial.println(F("Setting API details"));
  token=server.arg("token");
  set_pswd(token, token_offset, true);
  send_text(F("API details updated"));
}

void set_apiurl()
{
  Serial.println(F("Setting API URL"));
  if (state == LOCKED)
    return send_text(F("Safe is locked - Can not change now"));

  String newurl = server.arg("apiurl");
  if (newurl != "" && newurl != apiurl)
  {
    apiurl=newurl;
    set_pswd(apiurl, apiurl_offset);
    send_text(F("URL updated"));
  }
  else
    send_text(F("No update made"));
}

void enable_update(bool enable)
{
  if (enable)
  {
    if (lockid == "")
    {
      allow_updates = true;
      send_text(F("Updates can be sent using BasicOTA to: ") + WiFi.localIP().toString());
    }
    else
    {
      allow_updates = false;
      send_text(F("Can not perform update while safe is locked"));
    }
  }
  else
  {
    allow_updates = false;
    send_text(F("Update server disabled"));
  }
}

void pick_lock()
{
  Serial.println(F("Picking lock"));
  if (state == LOCKED)
    return send_text(F("Safe is already locked"));

  get_locks();

  if (good_api_result)
  {
    int i=raw_data.size();
    if (i == 0)
      return send_text(F("No locks available to pick"));

    String s=F("These locks are available:<p><form method=post action=safe/ enctype=\"multipart/form-data\">\n");

    for (int j=0;j<i;j++)
    {
      s += F("<input type=radio name=lockid value=") + html_clean(raw_data[j]["id"]) + ">" + html_clean(raw_data[j]["lockname"]) + F(", started at ") + html_clean(raw_data[j]["loadtime"]);
      String k = raw_data[j]["keyholder"];
      if ( k != "<self>")
        s += F(", with keyholder ") + html_clean(k);
      s += F("<br>");
    }
    s += F("<p><input type=Submit value=\"Start Lock\" name=set_lock></form>");

    return send_text(s);
  }
}

void set_lock()
{
  lockid=server.arg("lockid");
  Serial.println(F("Setting lock to ")+lockid);

  if (state == LOCKED)
    return send_text(F("Safe is already locked"));

  get_locks();
  if (!good_api_result)
    return;

  if (find_lock(lockid)==-1)
  {
    lockid="";
    state=UNLOCKED;
    return send_text(F("No lock found for ID ") + String(lockid) + F("!  Either the lockID is wrong or you deleted it..."));
  }
  set_pswd(lockid, lockid_offset);
  state=LOCKED;
  allow_updates = false;

  return send_text(F("Safe locked"));
}

/////////////////////////////////////////

boolean handleRequest()
{ 
  String path=server.uri();
  if (!wifi_connected)
  {
    // If we're in AP mode then all requests must go to change_ap
    // and there's no authn required
    ui_username = "";
    ui_pswd = "";
    path=F("/change_ap.html");
  }

  Serial.println(F("New client for >>>")+path+F("<<<"));

  for(int i=0;i<server.args();i++)
  {
    Serial.println(F("Arg ") + String(i) + F(": ") + server.argName(i) + F(" --- ") + server.arg(i));
  }

  // Ensure username/password have been passed
  if (ui_username != "" && !server.authenticate(ui_username.c_str(), ui_pswd.c_str()))
  {
    Serial.println(F("Bad authentication; login needed"));
    server.requestAuthentication(BASIC_AUTH,"safe");
    return true;
  }

       if (path == F("/"))                 { send_text(FPSTR(index_html)); }
  else if (path == F("/main_frame.html"))  { send_text(FPSTR(main_frame_html)); }
  else if (path == F("/menu_frame.html"))  { send_text(FPSTR(menu_frame_html)); }
  else if (path == F("/top_frame.html"))   { send_text(FPSTR(top_frame_html)); }
  else if (path == F("/change_auth.html")) { display_auth(); }
  else if (path == F("/change_ap.html"))   { set_ap(); }
  else if (path == F("/enable_update"))    { enable_update(1); }
  else if (path == F("/disable_update"))   { enable_update(0); }
  else if (path == F("/pick_lock"))        { pick_lock(); }
  else if (path == F("/renew"))            { renew(); }
  else if (path == F("/safe/"))
  {
         if (server.hasArg("status"))     { status(); }
         if (server.hasArg("version"))    { status(String(MODEL) + " " + VERSION + ","); }
    else if (server.hasArg("open"))       { opensafe(); }
    else if (server.hasArg("setauth"))    { set_auth(); }
    else if (server.hasArg("setapi"))     { set_api(); }
    else if (server.hasArg("setapiurl"))  { set_apiurl(); }
    else if (server.hasArg("set_lock"))   { set_lock(); }
    else return false;
  }
  else
  {
    Serial.println(F("File not found"));
    return false;
  }
  return true;
}

/////////////////////////////////////////
void PROGMEMprint(File f, const unsigned char str[])
{
  int x=SIZE_OF_CERTS;
  Serial.print("To write:");
  Serial.println(x);
  while(x--)
  {
    f.write(pgm_read_byte(str++));
  }
}

void setup()
{
  Serial.begin(115200);
  delay(500);

  Serial.println(F("Starting..."));

  // Get the EEPROM contents into RAM
  EEPROM.begin(EEPROM_SIZE);

  // Ensure LED is off
  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, HIGH);

  // To avoid needing to have a filesystem uploaded, we've embedded
  // the cert store into this code.  We'll write it out at start
  // time and then let BearSSL load it
  Serial.println("Opening filesystem");
  if (!FSTYPE.begin())
  {
    Serial.println("Failed!");
    return;
  }

  if (!FSTYPE.format())
  {
    Serial.println("Failed to format!");
    return;
  }

  Serial.println("Creating cert store");
  File f=FSTYPE.open("/certs.ar","w");
  if (!f)
  {
    Serial.println(F("Can not open file for writing"));
    return;
  }
  Serial.println("Writing");
  // NOTE: f.write(certs_ar);  doesn't work 'cos certs_ar is in PROGMEM 
  PROGMEMprint(f,certs_ar);
  Serial.println("Complete");
  f.close();

  FSInfo fs_info; 

  FSTYPE.info(fs_info);

  Serial.print("Total size of FS is: ");
  Serial.println(fs_info.totalBytes);

  Serial.print("Used size of FS is: ");
  Serial.println(fs_info.usedBytes);

  Serial.println("File list: ");
  Dir dir = FSTYPE.openDir("/");
  while (dir.next())
  {
    Serial.println(dir.fileName());
  }
  Serial.println("Finished");

  f=FSTYPE.open("/certs.ar","r");
  if (!f)
  {
    Serial.println(F("Can not open file for reading"));
  }
  else
  {
    Serial.print("Read length: ");
    Serial.println(f.size());
  }
  f.close();

  Serial.println("Loading cert store");
  int numCerts = certStore.initCertStore(FSTYPE, "/certs.idx", "/certs.ar");
  Serial.print("Number of CA certs read: ");
  Serial.println(numCerts);
  if (numCerts == 0)
  { 
    Serial.println(F("No certs found. Failed to instantiate filesystem!"));
    return; // Can't connect to anything w/o certs!
  }
  // Ensure that when we talk to Chaster we're validating the cert
  client.setCertStore(&certStore);
//  client.setInsecure();


  Serial.println(F("Getting passwords from EEPROM"));

  // Try reading the values from the EEPROM
  ui_username = get_pswd(ui_username_offset);
  ui_pswd     = get_pswd(ui_pswd_offset);
  wifi_ssid   = get_pswd(ui_wifi_ssid_offset);
  wifi_pswd   = get_pswd(ui_wifi_pswd_offset);
  token       = get_pswd(token_offset);
  lockid      = get_pswd(lockid_offset);
  apiurl      = get_pswd(apiurl_offset);
  safename    = get_pswd(safename_offset);
  pinstr      = get_pswd(pin_offset);

  if (apiurl == "")
    apiurl = DEFAULT_API;

  if (safename == "")
    safename=F("safe");

  if (pinstr != "")
    pin=pinstr.toInt();
  else
    pin=default_pin;

  // Set the safe state
  pinMode(pin, OUTPUT);
  digitalWrite(pin, LOW);

  if (lockid != "")
  { 
    state=LOCKED;
  }

  // This is a debugging line; it's only sent to the serial
  // port which can only be accessed when the safe is unlocked.
  // We don't exposed passwords!
  Serial.println(F("Found in EEPROM:"));
  Serial.println(F("  UI Username >>>")+ ui_username + F("<<<"));
  Serial.println(F("  UI Password >>>")+ ui_pswd + F("<<<"));
  Serial.println(F("  Wifi SSID   >>>")+ wifi_ssid + F("<<<"));
  Serial.println(F("  Wifi Pswd   >>>")+ wifi_pswd + F("<<<"));
  Serial.println(F("  Safe Name   >>>")+ safename + F("<<<"));
  Serial.println(F("  Dev Token   >>>")+ token + F("<<<"));
  Serial.println(F("  API URL     >>>")+ apiurl + F("<<<"));
  Serial.println(F("  LockID      >>>")+ lockid + F("<<<"));
  Serial.println(F("  Relay Pin   >>>")+ String(pin) + F("<<<"));

  // Connect to the network
  Serial.println();
  Serial.print("MAC: ");
  Serial.println(WiFi.macAddress());

  wifi_connected=false;
  if (wifi_ssid != "")
  {
    WiFi.mode(WIFI_STA);
    WiFi.begin(wifi_ssid, wifi_pswd);
    Serial.print("Connecting to ");
    Serial.print(wifi_ssid); Serial.println(" ...");

    // Wait for the Wi-Fi to connect.  Give up after 60 seconds and
    // let us fall into AP mode
    int i = 0;
    while (WiFi.status() != WL_CONNECTED && i < 60)
    {
      Serial.print(++i); Serial.print(' ');
      delay(1000);
    }
    Serial.println('\n');
    wifi_connected = (WiFi.status() == WL_CONNECTED);
  }

  if (wifi_connected)
  {
    Serial.println("Connection established!");  
    Serial.print("IP address:\t");
    Serial.println(WiFi.localIP());
    Serial.print("Hostname:\t");
    Serial.println(WiFi.hostname());

    // Get the current time.  We need this for TLS cert validation
    // it's not instant.  Timezone handling... GMT is always good :-)
    configTime(0, 0, "pool.ntp.org", "time.nist.gov");
    Serial.print("Waiting for NTP time sync: ");
    time_t now = time(nullptr);
    while (now < 8 * 3600 * 2)
    {
      delay(500);
      Serial.print(".");
      now = time(nullptr);
    }
    Serial.println("");
    struct tm timeinfo;
    gmtime_r(&now, &timeinfo);
    Serial.print("Current time (UTC): ");
    Serial.println(asctime(&timeinfo));
  }
  else
  {
    // Create an Access Point that mobile devices can connect to
    unsigned char mac[6];
    char macstr[7];
    WiFi.softAPmacAddress(mac);
    sprintf(macstr, "%02X%02X%02X", mac[3], mac[4], mac[5]);
    String AP_name="Safe-"+String(macstr);
    Serial.println("No connection; creating access point: "+AP_name);
    WiFi.mode(WIFI_AP);
    WiFi.softAP(AP_name);
  }

  //initialize mDNS service.
  MDNS.begin(safename);
  MDNS.addService("http", "tcp", 80);
  Serial.println("mDNS responder started");

  // This structure just lets us send all requests to the handler
  // If we can't handle it then send a 404 response
  server.onNotFound([]()
  {
    if (!handleRequest())
      server.send(404, "text/html", "Not found");
  });

  // Start TCP (HTTP) server
  server.begin();
  
  Serial.println("TCP server started");

  // Configure the OTA update service, but don't start it yet!
  ArduinoOTA.setHostname(safename.c_str());

  if (ui_pswd != "")
    ArduinoOTA.setPassword(ui_pswd.c_str());

  ArduinoOTA.onStart([]()
  {
    Serial.println(F("Starting updating"));
  });

  ArduinoOTA.onEnd([]() {
    Serial.println(F("\nEnd"));
  });

  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    Serial.printf("Progress: %u%%\r", (progress / (total / 100)));
  });

  ArduinoOTA.onError([](ota_error_t error) {
    Serial.printf("Error[%u]: ", error);
    if (error == OTA_AUTH_ERROR) Serial.println(F("Auth Failed"));
    else if (error == OTA_BEGIN_ERROR) Serial.println(F("Begin Failed"));
    else if (error == OTA_CONNECT_ERROR) Serial.println(F("Connect Failed"));
    else if (error == OTA_RECEIVE_ERROR) Serial.println(F("Receive Failed"));
    else if (error == OTA_END_ERROR) Serial.println(F("End Failed"));
  });

  ArduinoOTA.begin();

  Serial.println(F("OTA service configured"));
}

time_t last_check=0;

void loop()
{
  MDNS.update();
  server.handleClient();

  if (allow_updates)
    ArduinoOTA.handle();

  time_t now=time(NULL);
  if (now-last_check >= 12*60*60) // Every 12 hours
  {
    if (last_check != 0)
    {
      // Attempt to renew the developer token
      Serial.print("Renew token at: ");
      Serial.println(ctime(&now));
      renew(true);
    }
    last_check=now;
  }
}
