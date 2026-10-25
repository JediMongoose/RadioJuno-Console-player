// Code for RadioJuno player  ESP32S3 N16R8
// 
// Requires:
// ESP boards by expressif 2.0.14
// ESP32-audioI2S by schreibfaul1 2.0.0
// TFT_eSPI by Bodmer 2.5.43
//   User_Setup.h:
//      #define USER_SETUP_INFO "User_Setup"
//      #define ST7789_DRIVER      // Full configuration option, define additional parameters below for this display
//      #define TFT_MOSI     6
//      #define TFT_SCLK     7
//      #define TFT_CS       5
//      #define TFT_DC       4
//      #define TFT_RST      16
//      #define TFT_BL      -1
//      #define LOAD_GLCD   // Font 1. Original Adafruit 8 pixel font needs ~1820 bytes in FLASH
//      #define LOAD_FONT2  // Font 2. Small 16 pixel high font, needs ~3534 bytes in FLASH, 96 characters
//      #define LOAD_FONT4  // Font 4. Medium 26 pixel high font, needs ~5848 bytes in FLASH, 96 characters
//      #define LOAD_FONT6  // Font 6. Large 48 pixel font, needs ~2666 bytes in FLASH, only characters 1234567890:-.apm
//      #define LOAD_FONT7  // Font 7. 7 segment 48 pixel font, needs ~2438 bytes in FLASH, only characters 1234567890:-.
//      #define LOAD_FONT8  // Font 8. Large 75 pixel font needs ~3256 bytes in FLASH, only characters 1234567890:-.
//      #define LOAD_GFXFF  // FreeFonts. Include access to the 48 Adafruit_GFX free fonts FF1 to FF48 and custom fonts
//      #define SMOOTH_FONT
//      #define SPI_FREQUENCY  27000000
//      #define SPI_READ_FREQUENCY  20000000
//      #define SPI_TOUCH_FREQUENCY  2500000


#include "Arduino.h"
#include <string.h>
#include "WiFi.h"
#include "Audio.h"
#include <SPI.h>
#include <TFT_eSPI.h> 
#include <HTTPClient.h> 
#include "secrets.h"

// I2S PINS
#define I2S_BCLK 14
#define I2S_LRC  15
#define I2S_DOUT 13

// HARDWARE INPUT NODES (REVERSED RIGHT-SIDE ORDER)
#define BTN_START_STOP 42
#define BTN_BOOKMARK   41
#define BTN_SKIP       40
#define BTN_BLACKLIST  39

// htop Terminal Palette
#define HT_GREEN   0x07E0
#define HT_ORANGE  0xFBE0 
#define HT_BLUE    0x001F
#define HT_CYAN    0x07FF
#define HT_WHITE   0xFFFF
#define HT_BLACK   0x0000
#define HT_GRAY    0x39C7
#define HT_YELLOW  0xFFE0
#define HT_MAGENTA 0xF81F
#define HT_RED     0xF800 

TFT_eSPI tft = TFT_eSPI(); 
Audio audio;

// System Telemetry & Core Buffers
// NOTE: These used to be Arduino String objects. updateHtopDynamic() runs
// forever on a 500ms tick and was doing String concatenation on every pass
// (e.g. "Status: " + statusStr, "  " + trackLog[i]) - that's thousands of
// heap allocations per hour. On ESP32 that churn fragments the heap until
// an allocation eventually fails, which presents as the unit locking up
// after a while. Fixed-size char buffers avoid the churn entirely.
char currentArtist[64]   = "IDLE";
char currentTitle[128]   = "WAITING...";
char trackLog[5][80]     = {"-", "-", "-", "-", "-"};
char terminalMsg[32]     = "READY";

// State Machine Flags
bool triggerReconnect = false;
unsigned long reconnectDelay = 0;
bool slowStreamAlert = false; 
bool isMuted = false; // Tracks software mute state instead of pausing engine
bool isBookmarked = false;

// Confirmation Matrix Definitions
enum PendingAction { NONE, BOOKMARK, SKIP, BAN };
PendingAction currentAction = NONE;
bool isAwaitingConfirmation = false;

void setup() {
    Serial.begin(115200);
    delay(2000);
    Serial.println("\n[SYS] RADIOJUNO INITIALIZED");
    
    // Configure Pins with Internal Pullup Configuration
    pinMode(BTN_START_STOP, INPUT_PULLUP);
    pinMode(BTN_BOOKMARK, INPUT_PULLUP);
    pinMode(BTN_SKIP, INPUT_PULLUP);
    pinMode(BTN_BLACKLIST, INPUT_PULLUP);

    // Initial Display Sequence
    SPI.begin(TFT_SCLK, TFT_MISO, TFT_MOSI, TFT_CS);  // explicit bus init before tft.init()
    tft.init();
    tft.setRotation(1); 
    tft.invertDisplay(false); 
    tft.fillScreen(HT_BLACK);
    
    drawHtopStatic();

    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

    // Bounded wait instead of an infinite while(): if the AP is briefly
    // unavailable (e.g. right after a router hiccup) this used to spin
    // forever with no recovery path, which looks identical to a "frozen"
    // unit since it never reaches loop(). Retry the handshake every 20s
    // and self-reboot after 60s so the unit always recovers on its own.
    unsigned long wifiWaitStart = millis();
    unsigned long lastWifiRetry = wifiWaitStart;
    while (WiFi.status() != WL_CONNECTED) {
        delay(250);
        if (millis() - wifiWaitStart > 60000) {
            Serial.println("[SYS] WIFI TIMEOUT - REBOOTING");
            ESP.restart();
        }
        if (millis() - lastWifiRetry > 20000) {
            lastWifiRetry = millis();
            Serial.println("[SYS] WIFI RETRY...");
            WiFi.disconnect();
            WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
        }
    }

    audio.setPinout(I2S_BCLK, I2S_LRC, I2S_DOUT);
    audio.setVolume(21); 
    audio.connecttohost("http://185.227.110.132:61666/stream.mp3");
}

void loop() {
    audio.loop();
    handleButtons();

    // Throttled Dynamic Interface Render (Every 500ms)
    static unsigned long lastUpdate = 0;
    if (millis() - lastUpdate > 500) {
        lastUpdate = millis();
        updateHtopDynamic();
        audio.loop();  // extra pump - covers whatever time the redraw above used
    }

    // Passive Background Auto-Recovery Engine
    if (triggerReconnect && millis() > reconnectDelay) {
        triggerReconnect = false;
        Serial.println("[SYS] CLEARING CONGESTION / RE-HANDSHAKING...");
        audio.connecttohost("http://185.227.110.132:61666/stream.mp3");
    }

    // WiFi Link Watchdog: triggerReconnect only gets set from the audio
    // library's "wrong credentials" event, so if the STA link itself
    // drops (router hiccup, DHCP lease churn) nothing above ever notices.
    // Check every 5s and re-associate if the link is down.
    static unsigned long lastWifiCheck = 0;
    if (millis() - lastWifiCheck > 5000) {
        lastWifiCheck = millis();
        if (WiFi.status() != WL_CONNECTED) {
            Serial.println("[SYS] WIFI LINK DOWN - RECONNECTING");
            WiFi.disconnect();
            WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
        }
    }

    // Diagnostic: log free heap once a minute. Useful for confirming
    // memory stays flat over a long run instead of trending down.
    static unsigned long lastHeapLog = 0;
    if (millis() - lastHeapLog > 60000) {
        lastHeapLog = millis();
        Serial.printf("[SYS] Free heap: %u bytes\n", ESP.getFreeHeap());
    }
}

// ─── HELPER FOR ASYNC API TRIPS ───

void sendAPIRequest(const char* url) {
    if (WiFi.status() == WL_CONNECTED) {
        HTTPClient http;
        http.begin(url);
        http.setTimeout(150); 
        int httpCode = http.GET();
        if (httpCode > 0) {
            Serial.printf("[API] Success, Code: %d\n", httpCode);
        } else {
            Serial.printf("[API] Failed. Error: %s\n", http.errorToString(httpCode).c_str());
        }
        http.end();
    } else {
        Serial.println("[API] Request aborted: WiFi disconnected");
    }
}

// ─── HTOP UI STATIC RENDERING ───

void drawHtopStatic() {
    tft.fillScreen(HT_BLACK);
    
    // Cleaned Gauge Structure Labels
    tft.setTextColor(HT_WHITE);
    tft.drawString("WiFi", 10, 5, 1);
    tft.drawString("Buf ", 10, 15, 1);

    // Process List Sub-Header
    tft.fillRect(0, 95, 320, 15, HT_GREEN);
    tft.setTextColor(HT_BLACK);
    tft.setCursor(2, 98);
    tft.print("  TIME      TRACK INFO (LAST 5 PLAYED)");
}

// ─── HTOP UI DYNAMIC RENDERING ───

void updateHtopDynamic() {
    // Local State Trackers for the Footer Engine
    static bool lastMuted = false;
    static bool lastBookmarked = false;
    static bool lastAwaiting = false;
    static bool firstRun = true;

    // Dirty-flag trackers for the heavier sections below. These sections use
    // wide (300+ px) padded background fills with anti-aliased fonts
    // (SMOOTH_FONT), which is real SPI + render time - and this function was
    // redrawing all of it unconditionally every 500ms even when nothing had
    // changed. That periodic block of work was long enough to occasionally
    // delay the next audio.loop() call past what the I2S DMA buffer could
    // cover, which is what was causing the pops/crackles. Now these only
    // redraw when their content actually changes.
    static char lastArtist[sizeof(currentArtist)] = "";
    static char lastTitle[sizeof(currentTitle)] = "";
    static char lastStatusLine[24] = "";
    static char lastTrackLog[5][sizeof(trackLog[0])] = {"", "", "", "", ""};

    // 1. Current Live Audio Metadata Section (only redraw on change)
    if (strcmp(currentArtist, lastArtist) != 0) {
        tft.setTextPadding(310);
        tft.setTextColor(HT_CYAN, HT_BLACK);
        tft.drawString(currentArtist, 10, 45, 4);
        strncpy(lastArtist, currentArtist, sizeof(lastArtist) - 1);
        lastArtist[sizeof(lastArtist) - 1] = '\0';
    }
    if (strcmp(currentTitle, lastTitle) != 0) {
        tft.setTextPadding(310);
        tft.setTextColor(HT_WHITE, HT_BLACK);
        tft.drawString(currentTitle, 10, 70, 2);
        strncpy(lastTitle, currentTitle, sizeof(lastTitle) - 1);
        lastTitle[sizeof(lastTitle) - 1] = '\0';
    }

    // 2. WiFi Performance Gauge (Color-Adaptive Quality Scaling)
    int rssi = WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : -100;
    int rssiWidth = map(constrain(rssi, -100, -30), -100, -30, 0, 60);
    
    uint16_t wifiColor = HT_RED;
    if (rssi > -65)      wifiColor = HT_GREEN;  
    else if (rssi > -82) wifiColor = HT_ORANGE; 

    tft.fillRect(60, 5, rssiWidth, 8, wifiColor);
    tft.fillRect(60 + rssiWidth, 5, 60 - rssiWidth, 8, HT_GRAY);
    
    // 3. Audio Ring Buffer Gauge (Color-Adaptive Fill Scaling)
    uint32_t filledBytes = audio.inBufferFilled();
    uint32_t freeBytes   = audio.inBufferFree();
    uint32_t totalBytes  = filledBytes + freeBytes;
    int bufWidth = 0;
    
    if (totalBytes > 0) {
        bufWidth = (filledBytes * 60) / totalBytes;
        if (slowStreamAlert && bufWidth >= 24) {
            slowStreamAlert = false;
        }
    }
    
    uint16_t bufColor = HT_RED;
    if (bufWidth >= 36)      bufColor = HT_GREEN;  
    else if (bufWidth >= 15) bufColor = HT_ORANGE; 
    
    tft.fillRect(60, 15, bufWidth, 8, bufColor);   
    tft.fillRect(60 + bufWidth, 15, 60 - bufWidth, 8, HT_GRAY);

    // 4. Telemetry Block (Right-Justified Status & Uptime)
    const char* statusStr = "OFFLINE";
    uint16_t statusColor = HT_RED;
    
    if (WiFi.status() == WL_CONNECTED) {
        if (triggerReconnect) {
            statusStr = "CONNECTING";
            statusColor = HT_YELLOW;
        } else if (slowStreamAlert) {
            statusStr = "STALLING";
            statusColor = HT_MAGENTA;
        } else if (isMuted) {
            statusStr = "MUTED";
            statusColor = HT_WHITE;
        } else {
            statusStr = "PLAYING";
            statusColor = HT_GREEN;
        }
    }

    // Was "Status: " + statusStr (String concatenation) - runs every
    // 500ms forever, so this alone was ~7200 heap alloc/free cycles per
    // hour. A stack buffer + snprintf does the same job with zero heap use.
    char statusLine[24];
    snprintf(statusLine, sizeof(statusLine), "Status: %s", statusStr);
    if (strcmp(statusLine, lastStatusLine) != 0) {
        tft.setTextPadding(140);
        tft.setTextColor(statusColor, HT_BLACK);
        tft.drawRightString(statusLine, 310, 5, 1);
        strncpy(lastStatusLine, statusLine, sizeof(lastStatusLine) - 1);
        lastStatusLine[sizeof(lastStatusLine) - 1] = '\0';
    }

    char clockBuf[24];
    sprintf(clockBuf, "Uptime: %02lu:%02lu:%02lu", millis()/3600000, (millis()/60000)%60, (millis()/1000)%60);
    tft.setTextPadding(140);
    tft.setTextColor(HT_WHITE, HT_BLACK);
    tft.drawRightString(clockBuf, 310, 15, 1);

    // 5. Track History Table Display
    // Was building a new String per row per call ("  " + trackLog[i]) and
    // then repeatedly reallocating it character-by-character via
    // String::remove() inside the trim loop - 5 rows x 2 calls/sec, running
    // forever. Same idea below, just on a stack buffer instead of the heap.
    for (int i = 0; i < 5; i++) {
        if (strcmp(trackLog[i], lastTrackLog[i]) == 0) continue;
        strncpy(lastTrackLog[i], trackLog[i], sizeof(lastTrackLog[i]) - 1);
        lastTrackLog[i][sizeof(lastTrackLog[i]) - 1] = '\0';

        int y = 115 + (i * 15);
        tft.setTextColor(HT_GREEN, HT_BLACK);
        tft.setTextPadding(315);

        char logLine[84];
        snprintf(logLine, sizeof(logLine), "  %s", trackLog[i]);

        int len = strlen(logLine);
        while (len > 0 && tft.textWidth(logLine, 1) > 315) {
            logLine[--len] = '\0';
        }
        tft.drawString(logLine, 0, y, 1);
    }

    // 6. State-Driven Context Footer Render
    if (firstRun || isMuted != lastMuted || isBookmarked != lastBookmarked || isAwaitingConfirmation != lastAwaiting) {
        firstRun = false;
        lastMuted = isMuted;
        lastBookmarked = isBookmarked;
        lastAwaiting = isAwaitingConfirmation;

        if (isAwaitingConfirmation) {
            tft.fillRect(0, 225, 320, 15, HT_RED);
            tft.setTextColor(HT_WHITE, HT_RED);
            tft.setCursor(5, 228);
            tft.print(" Press [F4] to confirm");
        } else {
            tft.fillRect(0, 225, 320, 15, HT_CYAN);
            tft.setTextColor(HT_BLACK, HT_CYAN);
            tft.setCursor(5, 228);
            
            // F1: Mute/Unmute Label Adjustments
            if (!isMuted) {
                tft.print(" F1"); tft.setTextColor(HT_WHITE, HT_BLUE); tft.print("Mute"); tft.setTextColor(HT_BLACK, HT_CYAN); tft.print("/Unmute");
            } else {
                tft.print(" F1Mute/"); tft.setTextColor(HT_WHITE, HT_RED); tft.print("Unmute"); tft.setTextColor(HT_BLACK, HT_CYAN);
            }
            
            tft.print("  F2:Bookmark");
            if (isBookmarked) {
                tft.setTextColor(HT_BLUE, HT_CYAN); 
                tft.print("*");
                tft.setTextColor(HT_BLACK, HT_CYAN);
            } else {
                tft.print(" ");
            }
            
            tft.print(" F3:Skip  F4:Ban");
        }
    }
}

// ─── AUDIO ENGINE TELEMETRY CALLBACKS ───

// Trims leading/trailing whitespace from a char buffer in place
// (replacement for String::trim(), which reallocates on the heap)
void trimInPlace(char* str) {
    char* start = str;
    while (*start == ' ' || *start == '\t' || *start == '\n' || *start == '\r') start++;
    size_t len = strlen(start);
    if (start != str) memmove(str, start, len + 1);
    if (len == 0) return;
    char* end = str + len - 1;
    while (end >= str && (*end == ' ' || *end == '\t' || *end == '\n' || *end == '\r')) {
        *end = '\0';
        end--;
    }
}

void audio_showstreamtitle(const char* info) {
    Serial.printf("[METADATA] %s\n", info);
    if (strlen(info) < 3) return;

    isBookmarked = false;
    slowStreamAlert = false; 

    char timeBuffer[12];
    sprintf(timeBuffer, "%02lu:%02lu:%02lu", millis()/3600000, (millis()/60000)%60, (millis()/1000)%60);

    char combinedCurrent[200];
    snprintf(combinedCurrent, sizeof(combinedCurrent), "%s - %s", currentArtist, currentTitle);

    if (strcmp(info, combinedCurrent) != 0 && strcmp(combinedCurrent, "IDLE - WAITING...") != 0) {
        for (int i = 4; i > 0; i--) {
            strncpy(trackLog[i], trackLog[i-1], sizeof(trackLog[i]) - 1);
            trackLog[i][sizeof(trackLog[i]) - 1] = '\0';
        }
        snprintf(trackLog[0], sizeof(trackLog[0]), "%s    %s", timeBuffer, combinedCurrent);
    }

    const char* dash = strstr(info, " - ");
    if (dash != nullptr) {
        size_t artistLen = dash - info;
        if (artistLen >= sizeof(currentArtist)) artistLen = sizeof(currentArtist) - 1;
        strncpy(currentArtist, info, artistLen);
        currentArtist[artistLen] = '\0';
        strncpy(currentTitle, dash + 3, sizeof(currentTitle) - 1);
        currentTitle[sizeof(currentTitle) - 1] = '\0';
    } else {
        strncpy(currentArtist, "VARIOUS", sizeof(currentArtist) - 1);
        currentArtist[sizeof(currentArtist) - 1] = '\0';
        strncpy(currentTitle, info, sizeof(currentTitle) - 1);
        currentTitle[sizeof(currentTitle) - 1] = '\0';
    }
    trimInPlace(currentArtist);
    trimInPlace(currentTitle);
}

void audio_info(const char* info) {
    Serial.printf("[INFO] %s\n", info);

    if (strstr(info, "wrong credentials") != nullptr) {
        strncpy(terminalMsg, "401 AUTH_RETRY_3S", sizeof(terminalMsg) - 1);
        terminalMsg[sizeof(terminalMsg) - 1] = '\0';
        triggerReconnect = true;
        reconnectDelay = millis() + 3000; 
    } 
    else if (strstr(info, "slow stream") != nullptr) {
        strncpy(terminalMsg, "SLOW STREAM ALERT", sizeof(terminalMsg) - 1);
        terminalMsg[sizeof(terminalMsg) - 1] = '\0';
        slowStreamAlert = true; 
    } 
    else {
        strncpy(terminalMsg, info, sizeof(terminalMsg) - 1);
        terminalMsg[sizeof(terminalMsg) - 1] = '\0';
    }
}

// ─── DEBOUNCED INPUT INTERCEPT PROMPT PROCESSING ───

void handleButtons() {
    static unsigned long lastPress = 0;
    if (millis() - lastPress < 450) return; 

    bool b1 = (digitalRead(BTN_START_STOP) == LOW);
    bool b2 = (digitalRead(BTN_BOOKMARK) == LOW);
    bool b3 = (digitalRead(BTN_SKIP) == LOW);
    bool b4 = (digitalRead(BTN_BLACKLIST) == LOW);

    if (!b1 && !b2 && !b3 && !b4) return;

    if (isAwaitingConfirmation) {
        if (b4) { 
            Serial.println("[SYS] CONFIRMATION SIGNED -> DISPATCHING PAYLOAD");
            
            if (currentAction == BOOKMARK) {
                isBookmarked = !isBookmarked;
                if (isBookmarked) {
                    sendAPIRequest("http://pi3.local/api.php?bookmarkcurrent=1");
                } else {
                    sendAPIRequest("http://pi3.local/api.php?unbookmarkcurrent=1");
                }
            } 
            else if (currentAction == SKIP) {
                sendAPIRequest("http://pi3.local/api.php?skipcurrent=1");
            } 
            else if (currentAction == BAN) {
                sendAPIRequest("http://pi3.local/api.php?ban=1");
                sendAPIRequest("http://pi3.local/api.php?skipcurrent=1");
            }
        } else {
            Serial.println("[SYS] CONFIRMATION VOID -> INTERCEPT CANCELLED BY USER");
        }
        
        isAwaitingConfirmation = false;
        currentAction = NONE;
        lastPress = millis();
        return;
    }

    if (b1) {
        isMuted = !isMuted;
        if (isMuted) {
            audio.setVolume(0);  // Hardware level drop to total silence
        } else {
            audio.setVolume(21); // Restores runtime volume 
        }
        Serial.printf("[CMD] VOLUME AUDIO STATE: %s\n", isMuted ? "MUTED" : "UNMUTED");
        lastPress = millis();
    }
    else if (b2) {
        currentAction = BOOKMARK;
        isAwaitingConfirmation = true;
        lastPress = millis();
    }
    else if (b3) {
        currentAction = SKIP;
        isAwaitingConfirmation = true;
        lastPress = millis();
    }
    else if (b4) {
        currentAction = BAN;
        isAwaitingConfirmation = true;
        lastPress = millis();
    }
}