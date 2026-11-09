// ============================================================
//
//  SMART ALARM CLOCK
//  Arduino Uno | DFPlayer Mini | PCF8523 RTC | 20x4 I2C LCD
//
// ============================================================
//
//  HOW TO ADD NEW SONGS TO A PLAYLIST
//  ====================================
//
//  STEP 1 — FORMAT SD CARD as FAT32
//  STEP 2 — Create folder called exactly:  mp3  on the SD root
//  STEP 3 — Name files with 4-digit prefix: 0001.mp3, 0002.mp3, etc.
//           myDFPlayer.play(1) plays 0001.mp3, play(2) plays 0002.mp3
//  STEP 4 — Copy files to SD IN NUMERICAL ORDER one at a time
//  STEP 5 — Update playlist arrays below:
//
//    To add a song to Camp Wake Up:
//      int campPlaylist[] = {9,10,11,..., newTrackNumber};
//      int campSize       = 20;  // was 19, now 20
//
//    To add a brand new playlist:
//      1. int myPlaylist[] = {7, 8, 9};  int mySize = 3;
//      2. Add a row in drawPlaylistPicker()
//      3. Add an else-if in handleSelect() under PLAYLIST_PICKER
//      4. Increment PLAYLIST_COUNT
//
//  NOTES: MP3/WAV only. Max 255 files. Volume range 0-30.
//
// ============================================================


#include <SoftwareSerial.h>
#include <DFRobotDFPlayerMini.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <RTClib.h>


// ============================================================
// HARDWARE
// ============================================================

SoftwareSerial mySerial(10, 11);
DFRobotDFPlayerMini myDFPlayer;
LiquidCrystal_I2C lcd1(0x27, 20, 4);
RTC_PCF8523 rtc;


// ============================================================
// BUTTONS
// ============================================================

const int selectBtn = 2;
const int upBtn     = 4;
const int downBtn   = 3;
const int backBtn   = 5;


// ============================================================
// SCREEN STATE
// ============================================================
//
// Screen flow for playlists:
//
//   SONG_MENU
//     └─> PLAYLIST_ACTION_MENU   ("Play Now" or "Set as Alarm")
//           └─> PLAYLIST_PICKER  (shows Camp Wake Up / Lock In / etc.)
//                 └─> if came from "Play Now"  → shuffle + play, go to SONG_MENU
//                     if came from "Set Alarm" → set alarm playlist only, go to SONG_MENU

enum Screen {
  CLOCK_SCREEN,
  MAIN_MENU,
  ALARM_MENU,
  SOUND_MENU,
  SONG_MENU,
  PLAYLIST_ACTION_MENU,   // NEW: "Play Now" vs "Set as Alarm"
  PLAYLIST_PICKER         // renamed from PLAYLIST_MENU — picks which playlist
};

Screen currentScreen = CLOCK_SCREEN;


// ============================================================
// MAIN MENU
// ============================================================

const int MENU_SIZE = 4;
int menuIndex = 0;


// ============================================================
// BACKLIGHT
// ============================================================

bool backlightOn = true;


// ============================================================
// ALARM SETTINGS
// ============================================================

int  alarmHour12 = 7;
bool alarmIsAM   = true;
int  alarmMinute = 30;
int  alarmState  = 0;

bool alarmFired             = false;
int  lastAlarmMinuteChecked = -1;
bool alarmActive            = false;
unsigned long alarmRampLast = 0;
int  savedVol               = 10;


// ============================================================
// SOUND / VOLUME
// ============================================================

int currVol     = 10;
int lastDrawnVol = -1;  // tracks what volume the sound screen last showed
                        // so loop() can redraw it during alarm ramp


// ============================================================
// SONG PLAYBACK
// ============================================================

int  songMenuIndex       = 0;
int  currentSong         = 1;
bool isPlaying           = false;


// ============================================================
// PLAYLIST DEFINITIONS
// ============================================================
//
// campPlaylist: index 9 (track 0009.mp3... wait — the array index 9
// is campPlaylist[9] = track 18) — RE-READ: user said "song at index 9"
// meaning array index 9 of campPlaylist[], which is track number 18.
// That track plays first, then the rest shuffle randomly.
//
// lockPlaylist: fully shuffled every time it's activated.
//
// SHUFFLE_MAX must be >= the largest playlist size.

int campPlaylist[] = {9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27};
int campSize       = 19;

int lockPlaylist[] = {1,2,3,4,5,6,7,8};
int lockSize       = 8;

const int PLAYLIST_COUNT = 2;

// ---- ACTIVE PLAYBACK PLAYLIST ----
// What is currently playing (or will play when Play Now is selected).
int* playNowPlaylist  = campPlaylist;
int  playNowSize      = campSize;

// ---- ALARM PLAYLIST ----
// Separate from playback — Set as Alarm changes these without
// affecting what's currently playing.
int* alarmPlaylist    = campPlaylist;
int  alarmPlaylistSize = campSize;
bool alarmPlaylistIsCamp = true;  // true=camp(first-song fixed), false=lock(full shuffle)
bool playNowIsCamp       = true;

// ---- SHUFFLE STATE ----
// shuffleOrder[] holds the play order as indices into the active playlist.
// songIndexInPlaylist is our current position in shuffleOrder[].
// Both Play Now and Alarm use this same shuffle mechanism at activation time.

#define SHUFFLE_MAX 19   // must be >= largest playlist size

int shuffleOrder[SHUFFLE_MAX];     // play order for current playback
int alarmShuffleOrder[SHUFFLE_MAX]; // play order for alarm (set at alarm activation)
int songIndexInPlaylist = 0;

// ---- PLAYLIST ACTION CONTEXT ----
// Remembers whether the user entered PLAYLIST_PICKER via "Play Now" or "Set Alarm"
// so PLAYLIST_PICKER knows what to do when a playlist is selected.
enum PlaylistAction { ACTION_PLAY_NOW, ACTION_SET_ALARM };
PlaylistAction pendingAction = ACTION_PLAY_NOW;

int playlistActionIndex = 0;  // cursor in PLAYLIST_ACTION_MENU (0=Play Now, 1=Set Alarm)
int playlistPickerIndex = 0;  // cursor in PLAYLIST_PICKER


// ============================================================
// TIMING / DEBOUNCE
// ============================================================

unsigned long lastPress          = 0;
const unsigned long debounceTime = 180;


// ============================================================
// TIME CACHE — ATOMIC INTS
// ============================================================

int  timeHour24  = 0;
int  timeMinute  = 0;
int  timeSecond  = 0;
bool timeValid   = false;

unsigned long lastRtcRead             = 0;
const unsigned long RTC_READ_INTERVAL = 1000;


// ============================================================
// BIG DIGIT CLOCK — CUSTOM CHARACTERS
// ============================================================

byte charData[8][8] = {
  {31,31,31,31,31,31,31,31},
  {31,31,31, 0, 0, 0, 0, 0},
  { 0, 0, 0, 0, 0,31,31,31},
  {31,31,31,31, 0, 0, 0, 0},
  { 0, 0, 0, 0,31,31,31,31},
  {31,31, 0, 0, 0, 0,31,31},
  {31,31,31,31,31,31,31,31},
  { 0, 0,31,31,31,31, 0, 0}
};

const byte digitMap[10][9] = {
  { 0, 1, 0,  0,32, 0,  0, 2, 0},
  {32, 0,32, 32, 0,32, 32, 0,32},
  { 1, 1, 0, 32, 7,32,  0, 2, 2},
  { 1, 1, 0, 32, 7, 0,  2, 2, 0},
  { 0,32, 0,  0, 2, 0, 32,32, 0},
  { 0, 1, 1,  3, 7, 2,  2, 2, 0},
  { 0, 1,1,  0, 7, 0,  0, 2, 0},
  { 1, 1, 0, 32,32, 0, 32,32, 0},
  { 0, 1, 0,  0, 7, 0,  0, 2, 0},
  { 0, 1, 0,  3, 7, 0, 32,32, 0}
};

const int digitCols[4] = {0, 4, 10, 14};

int lastDrawnHour   = -1;
int lastDrawnMinute = -1;


// ============================================================
// SHUFFLE HELPERS
// ============================================================
//
// buildShuffle(): Fisher-Yates shuffle into a target array.
// For Camp playlist: campPlaylist[9] (track 18) goes first,
// then the remaining 18 tracks are shuffled into slots 1-18.
// For Lock playlist: all 8 tracks fully shuffled.
//
// Uses Arduino's random() — seed with analogRead(A0) in setup()
// for different order each power cycle.

void buildShuffle(int* playlist, int size, int* order, bool isCamp) {

  if (isCamp) {
    // Slot 0 = fixed: array index 9 of campPlaylist (track 18)
    order[0] = 0;  // index into playlist[]

    // Fill slots 1..size-1 with remaining indices (0-8, 10-18)
    int pos = 1;
    for (int i = 0; i < size; i++) {
      if (i != 9) order[pos++] = i;
    }

    // Fisher-Yates shuffle on slots 1..size-1 only
    for (int i = size - 1; i > 1; i--) {
      int j = random(1, i + 1);  // random index in range [1, i]
      int tmp = order[i];
      order[i] = order[j];
      order[j] = tmp;
    }

  } else {
    // Full shuffle: fill 0..size-1 sequentially then shuffle all
    for (int i = 0; i < size; i++) order[i] = i;

    for (int i = size - 1; i > 0; i--) {
      int j = random(0, i + 1);
      int tmp = order[i];
      order[i] = order[j];
      order[j] = tmp;
    }
  }
}


// ============================================================
// SETUP
// ============================================================

void setup() {

  Serial.begin(115200);
  mySerial.begin(9600);

  pinMode(selectBtn, INPUT_PULLUP);
  pinMode(upBtn,     INPUT_PULLUP);
  pinMode(downBtn,   INPUT_PULLUP);
  pinMode(backBtn,   INPUT_PULLUP);

  // Seed random number generator from floating analog pin
  randomSeed(analogRead(A0));

  lcd1.init();
  lcd1.backlight();

  // Register all 8 custom characters ONCE — never call createChar() again
  for (int i = 0; i < 8; i++) {
    lcd1.createChar(i, charData[i]);
  }

  if (!myDFPlayer.begin(mySerial)) {
    lcd1.clear();
    lcd1.setCursor(0, 0); lcd1.print("DFPLAYER ERROR");
    lcd1.setCursor(0, 1); lcd1.print("Check SD + wiring");
    while (true);
  }

  if (!rtc.begin()) {
    lcd1.clear();
    lcd1.setCursor(0, 0); lcd1.print("RTC ERROR");
    lcd1.setCursor(0, 1); lcd1.print("Check I2C wiring");
    while (true);
  }

  // ONE-TIME RTC SET: uncomment, upload, wait, re-comment, upload again.
  // rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));

  {
    DateTime t = rtc.now();
    timeHour24 = t.hour();
    timeMinute = t.minute();
    timeSecond = t.second();
    timeValid  = true;
  }
  lastRtcRead = millis();

  // Build initial shuffle orders for both playlists
  buildShuffle(campPlaylist, campSize, shuffleOrder,      true);
  buildShuffle(campPlaylist, campSize, alarmShuffleOrder, true);

  myDFPlayer.volume(currVol);
  drawClock();
}


// ============================================================
// MAIN LOOP
// ============================================================

void loop() {

  // ---- THROTTLED RTC READ ----
  if (millis() - lastRtcRead >= RTC_READ_INTERVAL) {
    lastRtcRead = millis();
    DateTime t  = rtc.now();
    timeHour24  = t.hour();
    timeMinute  = t.minute();
    timeSecond  = t.second();
  }

  handleButtons();

  // ---- LIVE CLOCK REFRESH ----
  if (currentScreen == CLOCK_SCREEN && timeValid) {
    if (timeHour24 != lastDrawnHour || timeMinute != lastDrawnMinute) {
      lastDrawnHour   = timeHour24;
      lastDrawnMinute = timeMinute;
      drawBigClock(timeHour24, timeMinute);
    }
  }

  // ---- LIVE VOLUME REFRESH ON SOUND SCREEN ----
  // During alarm ramp, currVol changes in loop() without a button press.
  // If the user is watching the sound screen, redraw it whenever currVol changes.
  if (currentScreen == SOUND_MENU && currVol != lastDrawnVol) {
    lastDrawnVol = currVol;
    drawSound();
  }

  // ---- ALARM VOLUME RAMP ----
  if (alarmActive) {
    if (millis() - alarmRampLast >= 3000) {
      alarmRampLast = millis();
      if (currVol < 15) {
        currVol++;
        myDFPlayer.volume(currVol);
      }
    }
  }

  // ---- ALARM TRIGGER CHECK ----
  if (timeMinute != lastAlarmMinuteChecked) {
    alarmFired = false;
    lastAlarmMinuteChecked = timeMinute;
  }

  int alarmHour24;
  if (alarmIsAM) {
    alarmHour24 = (alarmHour12 == 12) ? 0 : alarmHour12;
  } else {
    alarmHour24 = (alarmHour12 == 12) ? 12 : alarmHour12 + 12;
  }

  if (!alarmFired &&
      timeHour24 == alarmHour24 &&
      timeMinute == alarmMinute &&
      timeSecond <  5) {
    alarmFired = true;
    startAlarm();
  }
}


// ============================================================
// DRAW ONE BIG DIGIT
// ============================================================

void drawBigDigit(int num, int xPos) {
  if (num < 0 || num > 9) num = 0;
  for (int row = 0; row < 3; row++) {
    lcd1.setCursor(xPos, row);
    for (int col = 0; col < 3; col++) {
      byte c = digitMap[num][(row * 3) + col];
      if (c == 32) lcd1.print(' ');
      else         lcd1.write(c);
    }
  }
}


// ============================================================
// DRAW BIG CLOCK (rows 0-2) + ALARM ROW (row 3)
// ============================================================

void drawBigClock(int hour24, int minute) {

  if (hour24 < 0 || hour24 > 23) hour24 = 0;
  if (minute < 0 || minute > 59) minute = 0;

  bool isPM        = (hour24 >= 12);
  int  displayHour = hour24;
  if      (displayHour == 0)  displayHour = 12;
  else if (displayHour > 12)  displayHour -= 12;

  drawBigDigit(displayHour / 10, digitCols[0]);
  drawBigDigit(displayHour % 10, digitCols[1]);
  drawBigDigit(minute      / 10, digitCols[2]);
  drawBigDigit(minute      % 10, digitCols[3]);

  lcd1.setCursor(8, 0); lcd1.print('o');
  lcd1.setCursor(8, 1); lcd1.print(' ');
  lcd1.setCursor(8, 2); lcd1.print('o');

  lcd1.setCursor(17, 0); lcd1.print("   ");
  lcd1.setCursor(17, 1); lcd1.print(isPM ? " PM" : " AM");
  lcd1.setCursor(17, 2); lcd1.print("   ");

  lcd1.setCursor(0, 3);
  char abuf[21];
  snprintf(abuf, sizeof(abuf), "ALARM: %02d:%02d %s        ",
           alarmHour12, alarmMinute, alarmIsAM ? "AM" : "PM");
  lcd1.print(abuf);
}


// ============================================================
// DRAW CLOCK SCREEN
// ============================================================

void drawClock() {
  lcd1.clear();
  lastDrawnHour   = -1;
  lastDrawnMinute = -1;
  if (timeValid) {
    drawBigClock(timeHour24, timeMinute);
  }
}


// ============================================================
// START ALARM
// ============================================================
// Uses alarmPlaylist / alarmPlaylistSize / alarmShuffleOrder.
// Builds a fresh alarm shuffle so the order is new each day.

void startAlarm() {
  alarmActive   = true;
  alarmRampLast = millis();
  savedVol      = currVol;

  lcd1.backlight();

  // Rebuild alarm shuffle for a fresh order each time
  buildShuffle(alarmPlaylist, alarmPlaylistSize, alarmShuffleOrder, !alarmPlaylistIsCamp == false ? alarmPlaylistIsCamp : alarmPlaylistIsCamp);

  // Play first track in alarm shuffle order
  int firstIdx = alarmShuffleOrder[0];
  currentSong  = alarmPlaylist[firstIdx];
  myDFPlayer.play(currentSong);

  currVol = 1;
  myDFPlayer.volume(currVol);
  isPlaying = true;

  currentScreen = CLOCK_SCREEN;
  drawClock();
}


// ============================================================
// STOP ALARM
// ============================================================

void stopAlarm() {
  myDFPlayer.stop();
  isPlaying   = false;
  alarmActive = false;

  currVol = savedVol;
  myDFPlayer.volume(currVol);

  currentScreen = CLOCK_SCREEN;
  drawClock();
}


// ============================================================
// ACTIVATE PLAY NOW
// ============================================================
// Shuffles and begins playback of the selected playlist.
// Does NOT change alarmPlaylist.

void activatePlayNow(int* playlist, int size, bool isCamp) {
  playNowPlaylist = playlist;
  playNowSize     = size;
  playNowIsCamp   = isCamp;

  buildShuffle(playlist, size, shuffleOrder, isCamp);

  songIndexInPlaylist = 0;
  currentSong = playlist[shuffleOrder[0]];
  myDFPlayer.play(currentSong);
  isPlaying = true;
}


// ============================================================
// BUTTON HANDLER
// ============================================================

void handleButtons() {

  if (millis() - lastPress < debounceTime) return;

  // ---- BACK ----
  if (digitalRead(backBtn) == LOW) {
    lastPress = millis();
    if (alarmActive) {
      stopAlarm();
    } else {
      currentScreen = CLOCK_SCREEN;
      drawClock();
    }
    return;
  }

  // ---- SELECT ----
  else if (digitalRead(selectBtn) == LOW) {
    lastPress = millis();
    handleSelect();
    return;
  }

  // ---- UP / DOWN ----

  else if (currentScreen == MAIN_MENU) {
    if (digitalRead(upBtn) == LOW) {
      lastPress = millis();
      if (menuIndex > 0) menuIndex--;
      drawMenu();
    }
    else if (digitalRead(downBtn) == LOW) {
      lastPress = millis();
      if (menuIndex < MENU_SIZE - 1) menuIndex++;
      drawMenu();
    }
  }

  else if (currentScreen == ALARM_MENU) {
    if (digitalRead(upBtn) == LOW) {
      lastPress = millis();
      if      (alarmState == 0) alarmHour12 = (alarmHour12 % 12) + 1;
      else if (alarmState == 1) alarmMinute = (alarmMinute + 1) % 60;
      else if (alarmState == 2) alarmIsAM   = !alarmIsAM;
      drawAlarm();
    }
    else if (digitalRead(downBtn) == LOW) {
      lastPress = millis();
      if      (alarmState == 0) alarmHour12 = (alarmHour12 == 1) ? 12 : alarmHour12 - 1;
      else if (alarmState == 1) alarmMinute = (alarmMinute == 0) ? 59 : alarmMinute - 1;
      else if (alarmState == 2) alarmIsAM   = !alarmIsAM;
      drawAlarm();
    }
  }

  else if (currentScreen == SOUND_MENU) {
    if (digitalRead(upBtn) == LOW) {
      lastPress = millis();
      if (currVol < 30) currVol++;
      myDFPlayer.volume(currVol);
      drawSound();
    }
    else if (digitalRead(downBtn) == LOW) {
      lastPress = millis();
      if (currVol > 0) currVol--;
      myDFPlayer.volume(currVol);
      drawSound();
    }
  }

  else if (currentScreen == SONG_MENU) {
    if (digitalRead(upBtn) == LOW) {
      lastPress = millis();
      if (songMenuIndex > 0) songMenuIndex--;
      drawSongMenu();
    }
    else if (digitalRead(downBtn) == LOW) {
      lastPress = millis();
      if (songMenuIndex < 3) songMenuIndex++;
      drawSongMenu();
    }
  }

  else if (currentScreen == PLAYLIST_ACTION_MENU) {
    if (digitalRead(upBtn) == LOW) {
      lastPress = millis();
      if (playlistActionIndex > 0) playlistActionIndex--;
      drawPlaylistActionMenu();
    }
    else if (digitalRead(downBtn) == LOW) {
      lastPress = millis();
      if (playlistActionIndex < 1) playlistActionIndex++;
      drawPlaylistActionMenu();
    }
  }

  else if (currentScreen == PLAYLIST_PICKER) {
    if (digitalRead(upBtn) == LOW) {
      lastPress = millis();
      if (playlistPickerIndex > 0) playlistPickerIndex--;
      drawPlaylistPicker();
    }
    else if (digitalRead(downBtn) == LOW) {
      lastPress = millis();
      if (playlistPickerIndex < PLAYLIST_COUNT - 1) playlistPickerIndex++;
      drawPlaylistPicker();
    }
  }
}


// ============================================================
// SELECT LOGIC
// ============================================================

void handleSelect() {

  // ---- CLOCK → MAIN MENU ----
  if (currentScreen == CLOCK_SCREEN) {
    menuIndex = 0;
    currentScreen = MAIN_MENU;
    drawMenu();
    return;
  }

  // ---- MAIN MENU ----
  if (currentScreen == MAIN_MENU) {
    if (menuIndex == 0) {
      alarmState = 0;
      currentScreen = ALARM_MENU;
      drawAlarm();
    }
    else if (menuIndex == 1) {
      backlightOn = !backlightOn;
      if (backlightOn) lcd1.backlight();
      else             lcd1.noBacklight();
      drawMenu();
    }
    else if (menuIndex == 2) {
      currentScreen = SOUND_MENU;
      lastDrawnVol  = -1;  // force redraw so bar matches current vol
      drawSound();
    }
    else if (menuIndex == 3) {
      currentScreen = SONG_MENU;
      drawSongMenu();
    }
    return;
  }

  // ---- ALARM SETUP ----
  if (currentScreen == ALARM_MENU) {
    if (alarmState < 3) {
      alarmState++;
      drawAlarm();
    }
    else {
      alarmFired = false;
      currentScreen = MAIN_MENU;
      drawMenu();
    }
    return;
  }

  // ---- SOUND MENU ----
  if (currentScreen == SOUND_MENU) {
    currentScreen = MAIN_MENU;
    drawMenu();
    return;
  }

  // ---- SONG MENU ----
  if (currentScreen == SONG_MENU) {
    switch (songMenuIndex) {

      case 0:   // Play / Pause
        if (isPlaying) myDFPlayer.pause();
        else           myDFPlayer.start();
        isPlaying = !isPlaying;
        break;

      case 1:   // Next — advance through shuffleOrder, wrap to 0
        songIndexInPlaylist = (songIndexInPlaylist + 1) % playNowSize;
        currentSong = playNowPlaylist[shuffleOrder[songIndexInPlaylist]];
        myDFPlayer.play(currentSong);
        isPlaying = true;
        break;

      case 2:   // Previous — go back through shuffleOrder, wrap to end
        songIndexInPlaylist = (songIndexInPlaylist == 0) ? playNowSize - 1 : songIndexInPlaylist - 1;
        currentSong = playNowPlaylist[shuffleOrder[songIndexInPlaylist]];
        myDFPlayer.play(currentSong);
        isPlaying = true;
        break;

      case 3:   // Open playlist action menu
        playlistActionIndex = 0;
        currentScreen = PLAYLIST_ACTION_MENU;
        drawPlaylistActionMenu();
        return;
    }
    drawSongMenu();
    return;
  }

  // ---- PLAYLIST ACTION MENU ----
  // User picks "Play Now" or "Set as Alarm", then goes to picker
  if (currentScreen == PLAYLIST_ACTION_MENU) {
    pendingAction       = (playlistActionIndex == 0) ? ACTION_PLAY_NOW : ACTION_SET_ALARM;
    playlistPickerIndex = 0;
    currentScreen       = PLAYLIST_PICKER;
    drawPlaylistPicker();
    return;
  }

  // ---- PLAYLIST PICKER ----
  // Picks a specific playlist, executes the pending action
  if (currentScreen == PLAYLIST_PICKER) {

    // Resolve which playlist was picked
    int*  chosenPlaylist = campPlaylist;
    int   chosenSize     = campSize;
    bool  chosenIsCamp   = true;

    if (playlistPickerIndex == 0) {
      chosenPlaylist = campPlaylist;
      chosenSize     = campSize;
      chosenIsCamp   = true;
    }
    else if (playlistPickerIndex == 1) {
      chosenPlaylist = lockPlaylist;
      chosenSize     = lockSize;
      chosenIsCamp   = false;
    }
    // Add more playlists here:
    // else if (playlistPickerIndex == 2) { chosenPlaylist = myList; chosenSize = mySize; chosenIsCamp = false; }

    if (pendingAction == ACTION_PLAY_NOW) {
      // Shuffle and start playing immediately — does NOT change alarm playlist
      activatePlayNow(chosenPlaylist, chosenSize, chosenIsCamp);
      currentScreen = SONG_MENU;
      drawSongMenu();

    } else {
      // Set as alarm only — does NOT start playback
      alarmPlaylist        = chosenPlaylist;
      alarmPlaylistSize    = chosenSize;
      alarmPlaylistIsCamp  = chosenIsCamp;
      // Pre-build the alarm shuffle so it's ready when the alarm fires
      buildShuffle(alarmPlaylist, alarmPlaylistSize, alarmShuffleOrder, alarmPlaylistIsCamp);

      // Confirm to user then return to song menu
      lcd1.clear();
      lcd1.setCursor(0, 0); lcd1.print("Alarm playlist set!");
      lcd1.setCursor(0, 2); lcd1.print(chosenIsCamp ? "Camp Wake Up" : "Lock In");
      delay(1200);  // brief confirmation — only delay in the whole program
      currentScreen = SONG_MENU;
      drawSongMenu();
    }
    return;
  }
}


// ============================================================
// DISPLAY FUNCTIONS
// ============================================================


// ---- MAIN MENU ----
void drawMenu() {
  lcd1.clear();
  lcd1.setCursor(0, 0); lcd1.print(menuIndex == 0 ? "> Alarm"      : "  Alarm");
  lcd1.setCursor(0, 1); lcd1.print(menuIndex == 1 ? "> Brightness" : "  Brightness");
  lcd1.setCursor(0, 2); lcd1.print(menuIndex == 2 ? "> Sound"      : "  Sound");
  lcd1.setCursor(0, 3); lcd1.print(menuIndex == 3 ? "> Song"       : "  Song");
}

// ---- ALARM SETUP ----
void drawAlarm() {
  lcd1.clear();
  lcd1.setCursor(0, 0); lcd1.print("SET ALARM");

  lcd1.setCursor(0, 1);
  lcd1.print(alarmState == 0 ? "> HOUR: " : "  HOUR: ");
  char hbuf[3]; sprintf(hbuf, "%2d", alarmHour12); lcd1.print(hbuf);

  lcd1.setCursor(0, 2);
  lcd1.print(alarmState == 1 ? "> MIN:  " : "  MIN:  ");
  char mbuf[3]; sprintf(mbuf, "%02d", alarmMinute); lcd1.print(mbuf);

  lcd1.setCursor(0, 3);
  if      (alarmState == 2) { lcd1.print("> "); lcd1.print(alarmIsAM ? "AM" : "PM"); lcd1.print(" UP/DN=toggle"); }
  else if (alarmState == 3) { lcd1.print("  SELECT = SAVE"); }
  else                      { lcd1.print("  "); lcd1.print(alarmIsAM ? "AM" : "PM"); }
}

// ---- SOUND / VOLUME ----
// lastDrawnVol is updated by loop() so this can be called from both
// button presses and the live-update path without double-drawing.
void drawSound() {
  lcd1.clear();
  lcd1.setCursor(0, 0); lcd1.print("VOLUME: "); lcd1.print(currVol);

  lcd1.setCursor(0, 1);
  lcd1.print("[");
  int bars = currVol / 2;
  for (int i = 0; i < 15; i++) lcd1.print(i < bars ? '#' : '-');
  lcd1.print("]");

  lcd1.setCursor(0, 3); lcd1.print("SELECT = BACK");

  lastDrawnVol = currVol;  // sync so loop() doesn't immediately redraw
}

// ---- SONG MENU ----
void drawSongMenu() {
  lcd1.clear();
  lcd1.setCursor(0, 0); lcd1.print(songMenuIndex == 0 ? "> Play/Pause" : "  Play/Pause");
  lcd1.setCursor(0, 1); lcd1.print(songMenuIndex == 1 ? "> Next Song"  : "  Next Song");
  lcd1.setCursor(0, 2); lcd1.print(songMenuIndex == 2 ? "> Previous"   : "  Previous");
  lcd1.setCursor(0, 3); lcd1.print(songMenuIndex == 3 ? "> Playlists"  : "  Playlists");
}

// ---- PLAYLIST ACTION MENU ----
// "Play Now" or "Set as Alarm"
void drawPlaylistActionMenu() {
  lcd1.clear();
  lcd1.setCursor(0, 0); lcd1.print("PLAYLIST:");
  lcd1.setCursor(0, 1); lcd1.print(playlistActionIndex == 0 ? "> Play Now"    : "  Play Now");
  lcd1.setCursor(0, 2); lcd1.print(playlistActionIndex == 1 ? "> Set as Alarm": "  Set as Alarm");
}

// ---- PLAYLIST PICKER ----
// Shows the list of available playlists to choose from.
// To add a new playlist: add a row here, increment PLAYLIST_COUNT,
// add array+size+isCamp in the PLAYLIST_PICKER select block.
void drawPlaylistPicker() {
  lcd1.clear();
  lcd1.setCursor(0, 0); lcd1.print("SELECT PLAYLIST:");
  lcd1.setCursor(0, 1); lcd1.print(playlistPickerIndex == 0 ? "> Camp Wake Up" : "  Camp Wake Up");
  lcd1.setCursor(0, 2); lcd1.print(playlistPickerIndex == 1 ? "> Lock In"      : "  Lock In");
}
