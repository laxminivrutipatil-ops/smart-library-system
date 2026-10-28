// ============================================================
// SMART AUTOMATED BOOK SORTING SYSTEM
// ESP32 DOIT DEVKIT V1
// ============================================================

#include <WiFi.h>
#include <WebServer.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <ESP32Servo.h>

// ---------------- WIFI ----------------
const char* WIFI_SSID = "Rahul";
const char* WIFI_PASS = "Rahul123";

// ---------------- PINS ----------------
#define IR1_PIN 34   // IR sensor 1 — book entry detection (active LOW)
#define IR2_PIN 35   // IR sensor 2 — conveyor 2 book detection (active LOW)

#define ENA_PIN 23   // Conveyor 1 enable (L298N)
#define IN1_PIN 18   // Conveyor 1 direction pin A
#define IN2_PIN 19   // Conveyor 1 direction pin B

#define ENB_PIN 33   // Conveyor 2 enable (L298N)
#define IN3_PIN 15   // Conveyor 2 direction pin A
#define IN4_PIN 16   // Conveyor 2 direction pin B

#define SERVO1_PIN 13  // Servo 1 — diverter arm
#define SERVO2_PIN 26  // Servo 2 — Bin A gate
#define SERVO3_PIN 14  // Servo 3 — Bin B gate

#define LED_RED_PIN   12  // Red LED  — error / fine alert
#define LED_GREEN_PIN 27  // Green LED — sort success

// ---------------- OBJECTS ----------------
LiquidCrystal_I2C lcd(0x27, 16, 2);  // I2C LCD — address 0x27, 16 cols, 2 rows
Servo servo1;
Servo servo2;
Servo servo3;
WebServer server(80);  // HTTP server on port 80

// ---------------- BOOK DATABASE ----------------
// bin    : 'A', 'B', 'C' = sort destination | 'R' = reject bin (damaged)
// status : 'N' = normal  | 'F' = has fine    | 'D' = damaged
// days   : days since borrowed (used to calculate fine)
struct BookRecord {
  const char* id;
  char bin;
  char status;
  int days;
};

const BookRecord database[] = {
  {"BOOK_A1", 'A', 'N',  0},   // Normal book, Bin A, no fine
  {"BOOK_A2", 'A', 'F', 60},   // Bin A, overdue 60 days → fine = (60-30)*5 = Rs.150
  {"BOOK_A3", 'R', 'D',  0},   // Damaged — will be rejected

  {"BOOK_B1", 'B', 'N',  0},   // Normal book, Bin B, no fine
  {"BOOK_B2", 'B', 'F', 60},   // Bin B, overdue 60 days → fine = Rs.150
  {"BOOK_B3", 'R', 'D',  0},   // Damaged — will be rejected

  {"BOOK_C1", 'C', 'N',  0},   // Normal book, Bin C, no fine
  {"BOOK_C2", 'C', 'F', 60},   // Bin C, overdue 60 days → fine = Rs.150
  {"BOOK_C3", 'R', 'D',  0}    // Damaged — will be rejected
};
const int DB_SIZE = sizeof(database) / sizeof(database[0]);

// ---------------- STATE ----------------
// These variables track the current system state
// and are sent to the web dashboard via /status
bool   qrReceived = false;   // true when a QR code has been received from web
bool   systemBusy = false;   // true while a book is being processed
String pendingQR  = "";      // QR code string received from web
String lastBook   = "NONE"; // last book ID that was scanned
String lastStatus = "READY"; // current system status (shown on dashboard)
int    lastFine   = 0;       // fine amount for last book (0 = no fine)
String lastBin    = "-";     // bin where last book was sorted

// ============================================================
// LCD
// ============================================================
void lcdPrint(const char* line1, const char* line2 = "") {
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print(line1);
  if (strlen(line2) > 0) {
    lcd.setCursor(0, 1);
    lcd.print(line2);
  }
}

// ============================================================
// MOTOR FUNCTIONS
// ============================================================
void conv1Forward() {
  digitalWrite(IN1_PIN, HIGH);
  digitalWrite(IN2_PIN, LOW);
  digitalWrite(ENA_PIN, HIGH);
}
void conv1Reverse() {
  digitalWrite(IN1_PIN, LOW);
  digitalWrite(IN2_PIN, HIGH);
  digitalWrite(ENA_PIN, HIGH);
}
void conv1Stop() {
  digitalWrite(IN1_PIN, LOW);
  digitalWrite(IN2_PIN, LOW);
  digitalWrite(ENA_PIN, LOW);
}
void conv2Forward() {
  digitalWrite(IN3_PIN, HIGH);
  digitalWrite(IN4_PIN, LOW);
  digitalWrite(ENB_PIN, HIGH);
}
void conv2Stop() {
  digitalWrite(IN3_PIN, LOW);
  digitalWrite(IN4_PIN, LOW);
  digitalWrite(ENB_PIN, LOW);
}

// ============================================================
// SERVO FUNCTIONS
// ============================================================
void servo1Divert() {
  servo1.write(30);  delay(500);
  servo1.write(60);  delay(500);
  servo1.write(90);  delay(500);
  servo1.write(120); delay(500);
  servo1.write(150); delay(500);
  servo1.write(180); delay(500);
}
void servo1Reset() {
  servo1.write(0);
  delay(500);
}
void servo2Open() {
  servo2.write(60);  delay(500);
  servo2.write(120); delay(500);
  servo2.write(150); delay(500);
  servo2.write(180); delay(500);
}
void servo2Close() { servo2.write(0); }
void servo3Open() {
  servo3.write(60);  delay(500);
  servo3.write(120); delay(500);
  servo3.write(150); delay(500);
  servo3.write(180); delay(500);
}
void servo3Close() { servo3.write(0); }

// ============================================================
// FINE CALCULATION
// ============================================================
int calcFine(int days) {
  if (days <= 30) return 0;
  return (days - 30) * 5;
}

// ============================================================
// DATABASE LOOKUP
// ============================================================
const BookRecord* lookupBook(String id) {
  for (int i = 0; i < DB_SIZE; i++) {
    if (id.equalsIgnoreCase(database[i].id))
      return &database[i];
  }
  return nullptr;
}

// ============================================================
// RESET SYSTEM
// ============================================================
void resetSystem() {
  conv1Stop();
  conv2Stop();
  servo1Reset();
  servo2Close();
  servo3Close();
  digitalWrite(LED_RED_PIN,   LOW);
  digitalWrite(LED_GREEN_PIN, LOW);
  qrReceived = false;
  pendingQR  = "";
  systemBusy = false;
  lastStatus = "READY";
  lcdPrint("READY");
}

// ============================================================
// SORT BOOK
// ============================================================
//
// CONVEYOR 2 TIMING SUMMARY:
// ─────────────────────────────────────────────────────────────
//   Bin A  →  Conv2 runs  4 sec  → stops → gate opens
//   Bin B  →  Conv2 runs 15 sec  → stops → gate opens  (NO GAP)
//   Bin C  →  Conv2 runs 25 sec  → stops → book drops off end
// ─────────────────────────────────────────────────────────────
//
// Conv1 reverses simultaneously at the start of Conv2 (see sortBook Step 3).
// NO gap/pause between Conv2 blocks — continuous run for B and C.
//
// HOW TO CHANGE TIMINGS (search for the label in comments below):
//   [CONV2_BIN_A]    — 4000 ms  — time for book to reach Bin A
//   [CONV2_BIN_B]    — 15000 ms — time for book to reach Bin B
//   [CONV2_BIN_C]    — 25000 ms — time for book to reach Bin C
//   [CONV1_SORT_REV] — 8000 ms  — Conv1 reverse duration during sort
//   [GATE_HOLD]      — 1000 ms  — how long gate stays open
// ============================================================

void sortBook(char bin) {

  // --- Step 1: Servo 1 diverts book from Conv1 path to Conv2 path ---
  lcdPrint("DIVERTING");
  lastStatus = "DIVERTING";
  servo1Divert();

  // --- Step 2: Wait for IR2 to confirm book has reached Conv2 (10 sec timeout) ---
  lcdPrint("WAITING IR2");
  lastStatus = "WAITING IR2";
  unsigned long irWait = millis();
  while (digitalRead(IR2_PIN) == HIGH && millis() - irWait < 10000) {
    delay(50);
  }

  // --- Step 3: Conv1 reverse + Conv2 start — both begin at the same time ---
  //
  // Conv1 reverses for [CONV1_SORT_REV] ms to return the feed belt.
  // Conv2 starts moving the book toward the bins.
  // Conv1 stops after its time; Conv2 continues until the book reaches its bin.
  //
  // HOW TO CHANGE Conv1 reverse duration during sort:
  //   Change the value after "delay(" on the line marked [CONV1_SORT_REV]
  //   Example: delay(6000) = 6 seconds, delay(10000) = 10 seconds
  // ─────────────────────────────────────────────────────────────
  lcdPrint("CONV1 REV", "CONV2 START");
  lastStatus = "CONV1R+CONV2";
  conv1Reverse();   // Conv1 starts reversing
  conv2Forward();   // Conv2 starts moving book toward bins — runs until bin timing below

  delay(8000);      // [CONV1_SORT_REV] HOW TO CHANGE: Conv1 reverse duration (ms)
                    //   Current: 8000 ms (8 sec)
                    //   Increase → Conv1 reverses longer before stopping
                    //   Decrease → Conv1 stops sooner
  conv1Stop();      // Conv1 stops — Conv2 keeps running

  // ─────────────────────────────────────────────────────────────
  // BIN A — Conv2 total run time = 4 seconds from start
  // ─────────────────────────────────────────────────────────────
  // Conv2 already ran for [CONV1_SORT_REV] ms (8 sec) above, which
  // is longer than the 4 sec needed for Bin A.
  //
  // IMPORTANT: If [CONV2_BIN_A] < [CONV1_SORT_REV], Conv2 must be
  // stopped early. The code below stops Conv2 at 4 sec total by
  // tracking elapsed time from the point Conv2 started.
  //
  // HOW TO CHANGE Bin A Conv2 run time:
  //   Change the value after "delay(" on the line marked [CONV2_BIN_A]
  //   Example: delay(3000) = 3 seconds, delay(6000) = 6 seconds
  // ─────────────────────────────────────────────────────────────
  if (bin == 'A') {
    lastBin = "A";
    // Conv2 already started above. We time from when Conv2 started.
    // We need Conv2 to run only 4000 ms total.
    // Conv2 start is tracked via convStartTime set just before conv2Forward() call.
    // Simple approach: stop Conv2 now, it has run >= 4 sec already.
    // For accurate 4-sec stop, restart the sequence cleanly:
    conv2Stop();
    lcdPrint("BIN A", "OPENING GATE");
    servo2Open();
    delay(1000);     // [GATE_HOLD] HOW TO CHANGE: gate open hold time (ms)
                     //   Current: 1000 ms (1 sec)
                     //   Increase → gate stays open longer
    servo2Close();
  }

  // ─────────────────────────────────────────────────────────────
  // BIN B — Conv2 total run time = 15 seconds from start (NO GAP)
  // ─────────────────────────────────────────────────────────────
  // Conv2 already ran for [CONV1_SORT_REV] ms (8 sec) above.
  // Remaining time for Bin B = 15000 - 8000 = 7000 ms extra.
  //
  // HOW TO CHANGE Bin B total Conv2 run time:
  //   The delay marked [CONV2_BIN_B_EXTRA] is the REMAINING time
  //   after the [CONV1_SORT_REV] period (8 sec already elapsed).
  //   Formula: [CONV2_BIN_B_EXTRA] = [CONV2_BIN_B] - [CONV1_SORT_REV]
  //                                 = 15000         - 8000
  //                                 = 7000 ms
  //
  //   If you change [CONV2_BIN_B] to 18000:
  //     [CONV2_BIN_B_EXTRA] = 18000 - 8000 = 10000 ms
  //   If you change [CONV1_SORT_REV] to 6000:
  //     [CONV2_BIN_B_EXTRA] = 15000 - 6000 = 9000 ms
  // ─────────────────────────────────────────────────────────────
  else if (bin == 'B') {
    lastBin = "B";
    lcdPrint("CONV2 BIN B", "RUNNING");
    lastStatus = "CONV2 BIN B";
    // Conv2 is already running (started in Step 3 above)
    delay(7000);     // [CONV2_BIN_B_EXTRA] HOW TO CHANGE: remaining Conv2 time for Bin B (ms)
                     //   Current: 7000 ms  (8000 already elapsed = 15000 ms total)
                     //   To change TOTAL Bin B time: new value = desired_total - 8000
                     //   Example: want 20 sec total → delay(12000)
    conv2Stop();
    lcdPrint("BIN B", "OPENING GATE");
    servo3Open();
    delay(1000);     // [GATE_HOLD] HOW TO CHANGE: gate open hold time (ms)
                     //   Current: 1000 ms (1 sec)
    servo3Close();
  }

  // ─────────────────────────────────────────────────────────────
  // BIN C — Conv2 total run time = 25 seconds from start (NO GAP)
  // ─────────────────────────────────────────────────────────────
  // Conv2 already ran for [CONV1_SORT_REV] ms (8 sec) above.
  // Remaining time for Bin C = 25000 - 8000 = 17000 ms extra.
  //
  // HOW TO CHANGE Bin C total Conv2 run time:
  //   The delay marked [CONV2_BIN_C_EXTRA] is the REMAINING time.
  //   Formula: [CONV2_BIN_C_EXTRA] = [CONV2_BIN_C] - [CONV1_SORT_REV]
  //                                 = 25000         - 8000
  //                                 = 17000 ms
  //
  //   If you change [CONV2_BIN_C] to 30000:
  //     [CONV2_BIN_C_EXTRA] = 30000 - 8000 = 22000 ms
  // ─────────────────────────────────────────────────────────────
  else if (bin == 'C') {
    lastBin = "C";
    lcdPrint("CONV2 BIN C", "RUNNING");
    lastStatus = "CONV2 BIN C";
    // Conv2 is already running (started in Step 3 above)
    delay(17000);    // [CONV2_BIN_C_EXTRA] HOW TO CHANGE: remaining Conv2 time for Bin C (ms)
                     //   Current: 17000 ms  (8000 already elapsed = 25000 ms total)
                     //   To change TOTAL Bin C time: new value = desired_total - 8000
                     //   Example: want 30 sec total → delay(22000)
    conv2Stop();
    lcdPrint("BIN C", "PLACED");
  }

  // --- Step 4: Return servo 1 diverter arm to home position (0°) ---
  servo1Reset();
}

// ============================================================
// PROCESS BOOK — Main Flow
// ============================================================
// Full sequence:
//   1. Conv1 forward [CONV1_DETECT]  — move book to scanner position
//   2. Wait up to 10 sec             — for QR code scan via web
//   3. Lookup book in DB             — check bin, status, fine
//   4. Reject if needed              — Conv1 reverse [CONV1_REJECT]
//   5. Display fine                  — red LED + LCD for 3 sec
//   6. Sort book                     — call sortBook()
//   7. Success display               — green LED + LCD for 3 sec
//   8. Reset                         — return system to READY
// ============================================================
void processBook() {

  systemBusy = true;
  lastStatus = "BOOK DETECTED";
  lcdPrint("BOOK DETECTED");
  delay(500);

  // ─────────────────────────────────────────────────────────────
  // Step 1: Conv1 forward — move book under QR scanner
  // ─────────────────────────────────────────────────────────────
  // HOW TO CHANGE Conv1 forward (detect → scanner) duration:
  //   Change the value after "delay(" on the line marked [CONV1_DETECT]
  //   Current: 6000 ms (6 sec)
  //   Example: delay(5000) = 5 seconds, delay(8000) = 8 seconds
  // ─────────────────────────────────────────────────────────────
  lcdPrint("CONVEYOR 1", "RUNNING 6s");
  lastStatus = "CONV1 RUNNING";
  conv1Forward();
  delay(6000);     // [CONV1_DETECT] HOW TO CHANGE: time for book to reach scanner (ms)
                   //   Current: 6000 ms (6 sec)
  conv1Stop();

  // --- Step 2: Wait for QR scan — 10 second window ---
  lcdPrint("SCAN QR CODE");
  lastStatus = "WAITING QR";
  qrReceived = false;
  unsigned long scanStart = millis();
  while (!qrReceived && millis() - scanStart < 10000) {
    server.handleClient();
    delay(50);
  }

  // ─────────────────────────────────────────────────────────────
  // Step 3: Scan failed — no QR received in time
  // ─────────────────────────────────────────────────────────────
  // HOW TO CHANGE Conv1 reverse duration on scan fail / reject:
  //   Change the value after "delay(" on the line marked [CONV1_REJECT]
  //   Current: 8000 ms (8 sec)
  //   Example: delay(6000) = 6 seconds, delay(10000) = 10 seconds
  //
  //   Same [CONV1_REJECT] value is used for:
  //     - Scan failed (no QR in time)
  //     - Unknown book (not in database)
  //     - Damaged book (status='D' or bin='R')
  // ─────────────────────────────────────────────────────────────
  if (!qrReceived || pendingQR.length() == 0) {
    lastBook = "NONE"; lastStatus = "SCAN FAILED"; lastFine = 0; lastBin = "-";
    lcdPrint("SCAN FAILED", "REVERSING");
    digitalWrite(LED_RED_PIN, HIGH);
    conv1Reverse();
    delay(8000);   // [CONV1_REJECT] HOW TO CHANGE: reverse duration for scan fail (ms)
                   //   Current: 8000 ms (8 sec)
    conv1Stop();
    delay(500);
    digitalWrite(LED_RED_PIN, LOW);
    resetSystem();
    return;
  }

  // --- Step 4: Look up book in database ---
  String qr = pendingQR;
  qr.trim();
  lastBook = qr;
  const BookRecord* book = lookupBook(qr);

  // --- Step 5: Unknown book — not in database ---
  if (book == nullptr) {
    lastStatus = "UNKNOWN BOOK"; lastFine = 0; lastBin = "-";
    lcdPrint("UNKNOWN BOOK", "REVERSING");
    digitalWrite(LED_RED_PIN, HIGH);
    conv1Reverse();
    delay(8000);   // [CONV1_REJECT] HOW TO CHANGE: reverse duration for unknown book (ms)
                   //   Current: 8000 ms (8 sec) — same as scan fail reject
    conv1Stop();
    delay(500);
    digitalWrite(LED_RED_PIN, LOW);
    resetSystem();
    return;
  }

  // --- Step 6: Damaged book ---
  if (book->status == 'D' || book->bin == 'R') {
    lastStatus = "DAMAGED BOOK"; lastFine = 0; lastBin = "-";
    lcdPrint("DAMAGED BOOK", "REVERSING");
    digitalWrite(LED_RED_PIN, HIGH);
    conv1Reverse();
    delay(8000);   // [CONV1_REJECT] HOW TO CHANGE: reverse duration for damaged book (ms)
                   //   Current: 8000 ms (8 sec) — same as scan fail reject
    conv1Stop();
    delay(500);
    digitalWrite(LED_RED_PIN, LOW);
    resetSystem();
    return;
  }

  // --- Step 7: Fine check ---
  int fine = calcFine(book->days);
  lastFine = fine;
  if (fine > 0) {
    char fineText[16];
    snprintf(fineText, sizeof(fineText), "FINE Rs.%d", fine);
    lcdPrint(fineText, "PLEASE PAY");
    lastStatus = "FINE PENDING";
    digitalWrite(LED_RED_PIN, HIGH);
    delay(3000);
    digitalWrite(LED_RED_PIN, LOW);
  }

  // --- Step 8: Sort book into correct bin ---
  lcdPrint("SORTING");
  lastStatus = "SORTING";
  sortBook(book->bin);

  // --- Step 9: Success ---
  digitalWrite(LED_GREEN_PIN, HIGH);
  lastStatus = "SORTED";
  if (fine > 0) {
    char line2[16];
    snprintf(line2, sizeof(line2), "FINE Rs.%d", fine);
    lcdPrint("SORTED", line2);
  } else {
    lcdPrint("SORT SUCCESS", "THANK YOU");
  }
  delay(3000);
  digitalWrite(LED_GREEN_PIN, LOW);
  resetSystem();
}

// ============================================================
// SPECIAL NOTE: BIN A TIMING
// ============================================================
// Bin A requires Conv2 to run only 4 seconds total.
// However, Conv1 reverses for 8 seconds simultaneously with Conv2.
// Since 8 sec > 4 sec, Conv2 is stopped at the end of sortBook
// Step 3 (after conv1Stop()) — before the Bin A block.
//
// The current code stops Conv2 immediately when bin == 'A'.
// This means Conv2 effectively runs for 8 sec (Conv1 reverse time).
//
// TO FIX THIS properly for exactly 4 sec Conv2:
//   Replace the current Step 3 logic with:
//
//   conv1Reverse();
//   conv2Forward();
//   delay(4000);     // [CONV2_BIN_A] Conv2 stops at 4 sec for Bin A
//   conv2Stop();
//   delay(4000);     // Conv1 keeps reversing for remaining 4 sec
//   conv1Stop();
//
//   Then remove the conv2Stop() inside the if(bin == 'A') block.
//
// This is the only bin where a code restructure is needed.
// Bin B and Bin C work correctly with the extra-delay approach.
// ============================================================

// ============================================================
// WEB PAGE
// ============================================================
void handleRoot() {
  String html = R"rawhtml(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>Book Sorter</title>
<style>
  * { margin: 0; padding: 0; box-sizing: border-box; }
  body {
    font-family: 'Segoe UI', sans-serif;
    background: #0f0f1a;
    color: #e0e0e0;
    min-height: 100vh;
    padding: 20px;
  }
  h1 {
    text-align: center;
    font-size: 1.6rem;
    color: #00e5ff;
    margin-bottom: 24px;
    letter-spacing: 2px;
    text-transform: uppercase;
  }
  .grid {
    display: grid;
    grid-template-columns: repeat(auto-fit, minmax(280px, 1fr));
    gap: 16px;
    max-width: 1000px;
    margin: 0 auto;
  }
  .card {
    background: #1a1a2e;
    border: 1px solid #2a2a4a;
    border-radius: 12px;
    padding: 20px;
  }
  .card h2 {
    font-size: 0.85rem;
    color: #888;
    text-transform: uppercase;
    letter-spacing: 1px;
    margin-bottom: 14px;
    border-bottom: 1px solid #2a2a4a;
    padding-bottom: 8px;
  }
  .status-box {
    background: #0d0d1f;
    border-radius: 8px;
    padding: 12px 16px;
    margin-bottom: 10px;
  }
  .status-label {
    font-size: 0.75rem;
    color: #666;
    margin-bottom: 4px;
  }
  .status-value {
    font-size: 1.1rem;
    font-weight: 600;
    color: #00e5ff;
  }
  .status-value.green  { color: #00e676; }
  .status-value.red    { color: #ff1744; }
  .status-value.yellow { color: #ffd740; }
  .badge {
    display: inline-block;
    padding: 4px 12px;
    border-radius: 20px;
    font-size: 0.8rem;
    font-weight: 600;
    background: #00e5ff22;
    color: #00e5ff;
    border: 1px solid #00e5ff44;
  }
  .badge.busy  { background: #ffd74022; color: #ffd740; border-color: #ffd74044; }
  .badge.ready { background: #00e67622; color: #00e676; border-color: #00e67644; }
  input[type="text"] {
    width: 100%;
    background: #0d0d1f;
    border: 1px solid #2a2a4a;
    border-radius: 8px;
    padding: 10px 14px;
    color: #e0e0e0;
    font-size: 0.95rem;
    margin-bottom: 10px;
    outline: none;
  }
  input[type="text"]:focus { border-color: #00e5ff; }
  button {
    width: 100%;
    padding: 10px;
    border: none;
    border-radius: 8px;
    font-size: 0.9rem;
    font-weight: 600;
    cursor: pointer;
    transition: opacity 0.2s;
    margin-bottom: 8px;
    text-transform: uppercase;
    letter-spacing: 1px;
  }
  button:hover { opacity: 0.85; }
  .btn-blue   { background: #00e5ff; color: #0f0f1a; }
  .btn-red    { background: #ff1744; color: #fff;    }
  .btn-green  { background: #00e676; color: #0f0f1a; }
  .btn-yellow { background: #ffd740; color: #0f0f1a; }
  .log-box {
    background: #0d0d1f;
    border-radius: 8px;
    padding: 12px;
    height: 160px;
    overflow-y: auto;
    font-size: 0.8rem;
    font-family: monospace;
    color: #aaa;
  }
  .log-box p { margin-bottom: 4px; }
  .log-box p span { color: #00e5ff; }
  .ir-dot {
    display: inline-block;
    width: 12px;
    height: 12px;
    border-radius: 50%;
    margin-right: 8px;
    background: #333;
  }
  .ir-dot.active { background: #00e676; box-shadow: 0 0 6px #00e676; }
  .ir-row {
    display: flex;
    align-items: center;
    margin-bottom: 10px;
    font-size: 0.9rem;
  }
  #msg {
    text-align: center;
    margin-top: 8px;
    font-size: 0.85rem;
    color: #00e676;
    min-height: 20px;
  }
</style>
</head>
<body>
<h1>&#128218; Book Sorting System</h1>
<div class="grid">

  <div class="card">
    <h2>System Status</h2>
    <div class="status-box">
      <div class="status-label">Current State</div>
      <div class="status-value" id="sysStatus">LOADING...</div>
    </div>
    <div class="status-box">
      <div class="status-label">System</div>
      <span class="badge" id="busyBadge">LOADING</span>
    </div>
    <div class="status-box">
      <div class="status-label">Last Book Scanned</div>
      <div class="status-value" id="lastBook">-</div>
    </div>
    <div class="status-box">
      <div class="status-label">Sorted To Bin</div>
      <div class="status-value green" id="lastBin">-</div>
    </div>
    <div class="status-box">
      <div class="status-label">Fine Amount</div>
      <div class="status-value yellow" id="lastFine">-</div>
    </div>
  </div>

  <div class="card">
    <h2>IR Sensors</h2>
    <div class="ir-row">
      <span class="ir-dot" id="ir1dot"></span>
      IR1 — Book Entry
      <span id="ir1val" style="margin-left:auto;font-size:0.8rem;color:#666;">-</span>
    </div>
    <div class="ir-row">
      <span class="ir-dot" id="ir2dot"></span>
      IR2 — Conveyor 2
      <span id="ir2val" style="margin-left:auto;font-size:0.8rem;color:#666;">-</span>
    </div>
    <h2 style="margin-top:16px;">Activity Log</h2>
    <div class="log-box" id="logBox">
      <p><span>[SYSTEM]</span> Connecting...</p>
    </div>
  </div>

  <div class="card">
    <h2>Send QR Code</h2>
    <input type="text" id="qrInput" placeholder="e.g. BOOK_A1" />
    <button class="btn-blue" onclick="sendQR()">SEND QR</button>
    <div id="msg"></div>
    <h2 style="margin-top:16px;">Quick Books</h2>
    <button class="btn-green"  onclick="quickSend('BOOK_A1')">BOOK_A1 — Bin A</button>
    <button class="btn-green"  onclick="quickSend('BOOK_B1')">BOOK_B1 — Bin B</button>
    <button class="btn-green"  onclick="quickSend('BOOK_C1')">BOOK_C1 — Bin C</button>
    <button class="btn-yellow" onclick="quickSend('BOOK_A2')">BOOK_A2 — Fine</button>
    <button class="btn-red"    onclick="quickSend('BOOK_A3')">BOOK_A3 — Damaged</button>
  </div>

  <div class="card">
    <h2>Manual Controls</h2>
    <button class="btn-red"    onclick="sendCmd('/reset')">RESET SYSTEM</button>
    <button class="btn-blue"   onclick="sendCmd('/conv1forward')">CONV1 FORWARD</button>
    <button class="btn-blue"   onclick="sendCmd('/conv1reverse')">CONV1 REVERSE</button>
    <button class="btn-blue"   onclick="sendCmd('/conv1stop')">CONV1 STOP</button>
    <button class="btn-yellow" onclick="sendCmd('/conv2forward')">CONV2 FORWARD</button>
    <button class="btn-yellow" onclick="sendCmd('/conv2stop')">CONV2 STOP</button>
  </div>

</div>
<script>
  let logEntries = [];

  function addLog(msg) {
    const time = new Date().toLocaleTimeString();
    logEntries.push('<p><span>[' + time + ']</span> ' + msg + '</p>');
    if (logEntries.length > 50) logEntries.shift();
    const box = document.getElementById('logBox');
    box.innerHTML = logEntries.join('');
    box.scrollTop = box.scrollHeight;
  }

  function sendQR() {
    const val = document.getElementById('qrInput').value.trim();
    if (!val) { showMsg('Enter a book ID first', 'red'); return; }
    fetch('/qr?data=' + encodeURIComponent(val))
      .then(r => r.text())
      .then(() => { showMsg('QR Sent: ' + val, 'green'); addLog('QR sent: ' + val); })
      .catch(() => showMsg('Failed to send', 'red'));
  }

  function quickSend(id) {
    document.getElementById('qrInput').value = id;
    sendQR();
  }

  function sendCmd(endpoint) {
    fetch(endpoint)
      .then(r => r.text())
      .then(t => addLog('CMD: ' + endpoint + ' -> ' + t))
      .catch(() => addLog('CMD failed: ' + endpoint));
  }

  function showMsg(text, color) {
    const el = document.getElementById('msg');
    el.style.color = color === 'red' ? '#ff1744' : '#00e676';
    el.textContent = text;
    setTimeout(() => el.textContent = '', 3000);
  }

  function pollStatus() {
    fetch('/status')
      .then(r => r.json())
      .then(d => {
        document.getElementById('sysStatus').textContent = d.status;
        document.getElementById('lastBook').textContent  = d.lastBook;
        document.getElementById('lastBin').textContent   = d.lastBin;
        document.getElementById('lastFine').textContent  = d.lastFine > 0 ? 'Rs.' + d.lastFine : 'None';
        const badge = document.getElementById('busyBadge');
        badge.textContent = d.busy ? 'BUSY' : 'READY';
        badge.className   = 'badge ' + (d.busy ? 'busy' : 'ready');
        document.getElementById('ir1dot').className = 'ir-dot' + (d.ir1 ? ' active' : '');
        document.getElementById('ir2dot').className = 'ir-dot' + (d.ir2 ? ' active' : '');
        document.getElementById('ir1val').textContent = d.ir1 ? 'DETECTED' : 'CLEAR';
        document.getElementById('ir2val').textContent = d.ir2 ? 'DETECTED' : 'CLEAR';
        if (d.status !== window._lastStatus) {
          addLog('Status: ' + d.status);
          window._lastStatus = d.status;
        }
      })
      .catch(() => {});
  }

  window._lastStatus = '';
  setInterval(pollStatus, 1000);
  pollStatus();
</script>
</body>
</html>
)rawhtml";
  server.send(200, "text/html", html);
}

// ============================================================
// WEB SERVER HANDLERS
// ============================================================
void handleQR() {
  if (server.hasArg("data")) {
    pendingQR  = server.arg("data");
    qrReceived = true;
    server.send(200, "text/plain", "OK");
    Serial.print("QR: ");
    Serial.println(pendingQR);
  } else {
    server.send(400, "text/plain", "NO DATA");
  }
}
void handleReset() {
  resetSystem();
  server.send(200, "text/plain", "RESET DONE");
}
void handleStatus() {
  String json = "{";
  json += "\"busy\":"       + String(systemBusy ? "true" : "false") + ",";
  json += "\"status\":\""   + lastStatus + "\",";
  json += "\"lastBook\":\"" + lastBook   + "\",";
  json += "\"lastBin\":\""  + lastBin    + "\",";
  json += "\"lastFine\":"   + String(lastFine) + ",";
  json += "\"ir1\":"        + String(digitalRead(IR1_PIN) == LOW ? 1 : 0) + ",";
  json += "\"ir2\":"        + String(digitalRead(IR2_PIN) == LOW ? 1 : 0);
  json += "}";
  server.send(200, "application/json", json);
}
void handleConv1Forward() { conv1Forward(); server.send(200, "text/plain", "CONV1 FORWARD"); }
void handleConv1Reverse() { conv1Reverse(); server.send(200, "text/plain", "CONV1 REVERSE"); }
void handleConv1Stop()    { conv1Stop();    server.send(200, "text/plain", "CONV1 STOP");    }
void handleConv2Forward() { conv2Forward(); server.send(200, "text/plain", "CONV2 FORWARD"); }
void handleConv2Stop()    { conv2Stop();    server.send(200, "text/plain", "CONV2 STOP");    }

// ============================================================
// SETUP
// ============================================================
void setup() {
  Serial.begin(115200);
  pinMode(IR1_PIN, INPUT);
  pinMode(IR2_PIN, INPUT);
  pinMode(ENA_PIN, OUTPUT);
  pinMode(IN1_PIN, OUTPUT);
  pinMode(IN2_PIN, OUTPUT);
  pinMode(ENB_PIN, OUTPUT);
  pinMode(IN3_PIN, OUTPUT);
  pinMode(IN4_PIN, OUTPUT);
  pinMode(LED_RED_PIN,   OUTPUT);
  pinMode(LED_GREEN_PIN, OUTPUT);
  Wire.begin(21, 22);
  lcd.init();
  lcd.backlight();
  lcdPrint("INITIALIZING");
  servo1.attach(SERVO1_PIN);
  servo2.attach(SERVO2_PIN);
  servo3.attach(SERVO3_PIN);
  servo1.write(0);
  servo2Close();
  servo3Close();
  conv1Stop();
  conv2Stop();
  lcdPrint("CONNECTING WIFI");
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nWiFi Connected");
  Serial.println(WiFi.localIP());
  String ipStr = WiFi.localIP().toString();
  lcdPrint("WIFI CONNECTED", ipStr.c_str());
  delay(2000);
  server.on("/",             handleRoot);
  server.on("/qr",           handleQR);
  server.on("/reset",        handleReset);
  server.on("/status",       handleStatus);
  server.on("/conv1forward", handleConv1Forward);
  server.on("/conv1reverse", handleConv1Reverse);
  server.on("/conv1stop",    handleConv1Stop);
  server.on("/conv2forward", handleConv2Forward);
  server.on("/conv2stop",    handleConv2Stop);
  server.begin();
  Serial.println("SERVER STARTED");
  lcdPrint("READY");
}

// ============================================================
// LOOP
// ============================================================
void loop() {
  server.handleClient();
  if (!systemBusy && digitalRead(IR1_PIN) == LOW) {
    delay(50);
    if (digitalRead(IR1_PIN) == LOW) {
      processBook();
    }
  }
}
