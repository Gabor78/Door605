#include <Adafruit_GFX.h>
#include <Adafruit_NeoMatrix.h>

#define STREAM_MAX_LEN 8      // egy lefolyó karakterlánc legfeljebb ennyi karakter
#define MAX_COLS       32     // ennyi karakteroszlop lehet legfeljebb (1x1-nél minden pixeloszlop)
#define MAX_CELL_ROWS  8

struct EyeStyle {
  const char* name;
  uint8_t core[3];   // a pupilla körüli legfényesebb gyűrű
  uint8_t mid[3];    // az írisz közepe
  uint8_t edge[3];   // az írisz széle (sötét)
  float anger;       // >0 dühös (befelé lejtő szemhéj), <0 szomorú, 0 semleges
  bool slitPupil;    // függőleges macska-pupilla
  bool sclera;       // emberi szem: szürke átmenetes szemfehérje, benne kisebb írisz
  float irisR;       // az írisz sugara
  float pupilR;      // a pupilla sugara
};

// Egy lefolyó karakterlánc
struct GlyphStream {
  float y;                          // a fej-karakter felső sora (pixelben), lefelé nő
  float speed;                      // pixel / képkocka
  uint8_t len;                      // hány karakterből áll a lánc
  uint8_t glyph[STREAM_MAX_LEN];    // a karakterek (0 = fej)
  float dim;                        // fényerő-szorzó: 1 = előtér, kisebb = halvány háttér
  bool whiteHead;                   // fehér fej (csak az előtérben)
  bool active;
  unsigned long restartAt;
};

// A becsapódó kiírás egy jegye
struct SlamChar {
  const uint8_t* rows;
  uint8_t w;
  int8_t x;          // végső helye
  bool landed;
};

// A "zárlatos" villogás fázisai
enum GlitchPhase { GL_NONE, GL_OUT, GL_ALT, GL_IN };

// Elrendezés: hol van a "605"
struct DoorLayout {
  const char* name;
  bool right;                   // true: jobbra zárva, false: középen
};

// Egy karakterméret beállításai
struct RainSize {
  uint8_t n;          // a karakter mérete (n x n pixel)
  uint8_t vpitch;     // függőleges lépés a láncban (karakter + rés)
  uint8_t hpitch;     // vízszintes lépés az oszlopok között
  uint8_t minLen, maxLen;   // a lánc hossza (karakterben)
  uint8_t spMin, spMax;     // sebesség, század pixel / képkocka
  float trailDecay;         // az utánvilágítás lecsengése képkockánként
};

unsigned long lastIdleFrame = 0;

// ============================================================
// WS2812B 8x32 RGB mátrix
// ============================================================
#define MATRIX_PIN    16
#define MATRIX_WIDTH  32
#define MATRIX_HEIGHT 8

Adafruit_NeoMatrix matrix = Adafruit_NeoMatrix(
  MATRIX_WIDTH, MATRIX_HEIGHT, MATRIX_PIN,
  NEO_MATRIX_TOP + NEO_MATRIX_LEFT + NEO_MATRIX_COLUMNS + NEO_MATRIX_ZIGZAG,
  NEO_GRB + NEO_KHZ800
);

const uint8_t MATRIX_BRIGHTNESS = 30;   // 0-255; a színátmenetekhez 25-50 az ideális
const uint8_t MOTION_PIN = 33;          // RCWL-0516 OUT

// ============================================================
// Segédfüggvények
// ============================================================
void formatUptime(unsigned long ms, char* buf, size_t bufSize) {
  unsigned long totalSeconds = ms / 1000;
  snprintf(buf, bufSize, "%02lu:%02lu:%02lu.%03lu",
           totalSeconds / 3600, (totalSeconds / 60) % 60, totalSeconds % 60, ms % 1000);
}

void logLine(const char* format, ...) {
  char ts[16];
  formatUptime(millis(), ts, sizeof(ts));
  char msg[128];
  va_list args;
  va_start(args, format);
  vsnprintf(msg, sizeof(msg), format, args);
  va_end(args);
  Serial.printf("[%s] %s\n", ts, msg);
}

void showFrame() {
  matrix.show();
}

void px(int x, int y, uint8_t r, uint8_t g, uint8_t b) {
  matrix.setPassThruColor(((uint32_t)r << 16) | ((uint32_t)g << 8) | b);
  matrix.drawPixel(x, y, 0);
  matrix.setPassThruColor();
}

void pxf(int x, int y, float r, float g, float b) {   // float színnel, levágással
  px(x, y, (uint8_t)constrain(r, 0.0f, 255.0f), (uint8_t)constrain(g, 0.0f, 255.0f), (uint8_t)constrain(b, 0.0f, 255.0f));
}

void hsv(float h, float s, float v, uint8_t& r, uint8_t& g, uint8_t& b) {
  h -= floorf(h);
  float hh = h * 6.0f;
  int i = (int)hh;
  float f = hh - i;
  float p = v * (1 - s), q = v * (1 - s * f), t = v * (1 - s * (1 - f));
  float R, G, B;
  switch (i) {
    case 0:  R = v; G = t; B = p; break;
    case 1:  R = q; G = v; B = p; break;
    case 2:  R = p; G = v; B = t; break;
    case 3:  R = p; G = q; B = v; break;
    case 4:  R = t; G = p; B = v; break;
    default: R = v; G = p; B = q; break;
  }
  r = R * 255; g = G * 255; b = B * 255;
}

static inline float clamp01(float v) { return v < 0 ? 0 : (v > 1 ? 1 : v); }
static inline float lerpf(float a, float b, float t) { return a + (b - a) * t; }

// ============================================================
// "605" - szivárványos szín, állandó fényerő
// ============================================================
const uint16_t IDLE_FRAME_MS = 40;            // ~25 fps
const unsigned long HUE_PERIOD_MS = 48000;    // ennyi idő alatt fut végig az összes színen

// --- Vékony, 8 sor magas jelek (a legfelső bit a bal szélső oszlop) ---
const uint8_t GLYPH_6[8]     = { 0b0110, 0b1000, 0b1000, 0b1110, 0b1001, 0b1001, 0b1001, 0b0110 };
const uint8_t GLYPH_0[8]     = { 0b0110, 0b1001, 0b1001, 0b1001, 0b1001, 0b1001, 0b1001, 0b0110 };
const uint8_t GLYPH_5[8]     = { 0b1111, 0b1000, 0b1000, 0b1110, 0b0001, 0b0001, 0b1001, 0b0110 };
const uint8_t GLYPH_1[8]     = { 0b010, 0b110, 0b010, 0b010, 0b010, 0b010, 0b010, 0b111 };
const uint8_t GLYPH_2[8]     = { 0b0110, 0b1001, 0b0001, 0b0010, 0b0100, 0b1000, 0b1000, 0b1111 };
const uint8_t GLYPH_X[8]     = { 0, 0, 0, 0b101, 0b101, 0b010, 0b101, 0b101 };
const uint8_t GLYPH_MINUS[8] = { 0, 0, 0, 0, 0b111, 0, 0, 0 };
const uint8_t GLYPH_DOT[8]   = { 0, 0, 0, 0, 0, 0, 0, 0b1 };
const uint8_t GLYPH_D[8]     = { 0b1110, 0b1001, 0b1001, 0b1001, 0b1001, 0b1001, 0b1001, 0b1110 };
const uint8_t GLYPH_C[8]     = { 0b0110, 0b1001, 0b1000, 0b1000, 0b1000, 0b1000, 0b1001, 0b0110 };
const uint8_t GLYPH_V[8]     = { 0b10001, 0b10001, 0b10001, 0b01010, 0b01010, 0b01010, 0b00100, 0b00100 };

// --- Vastag (2 px vonalú) jelek ---
const uint8_t BOLD_6[8]     = { 0b011111, 0b111111, 0b110000, 0b111110, 0b111111, 0b110011, 0b111111, 0b011110 };
const uint8_t BOLD_0[8]     = { 0b011110, 0b111111, 0b110011, 0b110011, 0b110011, 0b110011, 0b111111, 0b011110 };
const uint8_t BOLD_5[8]     = { 0b111111, 0b111111, 0b110000, 0b111110, 0b111111, 0b000011, 0b111111, 0b111110 };
const uint8_t BOLD_1[8]     = { 0b0110, 0b1110, 0b0110, 0b0110, 0b0110, 0b0110, 0b0110, 0b1111 };
const uint8_t BOLD_2[8]     = { 0b011110, 0b111111, 0b000011, 0b001110, 0b011100, 0b110000, 0b111111, 0b111111 };
const uint8_t BOLD_X[8]     = { 0, 0, 0b11011, 0b11011, 0b01110, 0b01110, 0b11011, 0b11011 };
const uint8_t BOLD_MINUS[8] = { 0, 0, 0, 0b111, 0b111, 0, 0, 0 };
const uint8_t BOLD_DOT[8]   = { 0, 0, 0, 0, 0, 0, 0b11, 0b11 };
const uint8_t BOLD_D[8]     = { 0b111110, 0b111111, 0b110011, 0b110011, 0b110011, 0b110011, 0b111111, 0b111110 };
const uint8_t BOLD_C[8]     = { 0b011111, 0b111111, 0b110000, 0b110000, 0b110000, 0b110000, 0b111111, 0b011111 };
const uint8_t BOLD_V[8]     = { 0b1100011, 0b1100011, 0b0110110, 0b0110110, 0b0110110, 0b0011100, 0b0011100, 0b0001000 };

bool numberBold = false;   // vastag vagy vékony betűkkel írjuk a számot

// 8 sor magas jelek; w: szélesség
const uint8_t* bigGlyph(char ch, uint8_t& w) {
  if (numberBold) {
    switch (ch) {
      case '0': w = 6; return BOLD_0;
      case '1': w = 4; return BOLD_1;
      case '2': w = 6; return BOLD_2;
      case '5': w = 6; return BOLD_5;
      case '6': w = 6; return BOLD_6;
      case 'x': w = 5; return BOLD_X;
      case '-': w = 3; return BOLD_MINUS;
      case '.': w = 2; return BOLD_DOT;
      case 'D': w = 6; return BOLD_D;
      case 'C': w = 6; return BOLD_C;
      case 'V': w = 7; return BOLD_V;
      default:  w = 0; return nullptr;
    }
  }
  switch (ch) {
    case '0': w = 4; return GLYPH_0;
    case '1': w = 3; return GLYPH_1;
    case '2': w = 4; return GLYPH_2;
    case '5': w = 4; return GLYPH_5;
    case '6': w = 4; return GLYPH_6;
    case 'x': w = 3; return GLYPH_X;
    case '-': w = 3; return GLYPH_MINUS;
    case '.': w = 1; return GLYPH_DOT;
    case 'D': w = 4; return GLYPH_D;
    case 'C': w = 4; return GLYPH_C;
    case 'V': w = 5; return GLYPH_V;
    default:  w = 0; return nullptr;
  }
}

int bigTextWidth(const char* s) {
  int w = 0;
  for (; *s; s++) {
    uint8_t gw;
    if (!bigGlyph(*s, gw)) continue;
    if (w) w += 1;                 // 1 px rés
    w += gw;
  }
  return w;
}

const DoorLayout LAYOUTS[] = {
  { "right",  true  },    // szám jobbra zárva
/*  { "center", false },    // szám középen
  { "right",  true  },    // szám jobbra zárva*/
};
int layoutIndex = 0;

const char* const DOOR_TEXT = "605";

// A szám helye és pixelei (a layoutNumber() számolja ki az aktuális elrendezéshez és betűhöz)
int numLeft = 0, numWidth = 0;
bool numMask[MATRIX_WIDTH][MATRIX_HEIGHT];

// Egy szöveg kirajzolása maszkba (ha mask != nullptr) vagy a kijelzőre
void textToMask(const char* s, int x, bool mask[MATRIX_WIDTH][MATRIX_HEIGHT]) {
  for (; *s; s++) {
    uint8_t gw;
    const uint8_t* rows = bigGlyph(*s, gw);
    if (!rows) continue;
    for (int y = 0; y < MATRIX_HEIGHT; y++)
      for (int c = 0; c < gw; c++)
        if ((rows[y] & (1 << (gw - 1 - c))) && x + c >= 0 && x + c < MATRIX_WIDTH) mask[x + c][y] = true;
    x += gw + 1;
  }
}

void layoutNumber() {
  numWidth = bigTextWidth(DOOR_TEXT);
  numLeft = LAYOUTS[layoutIndex].right ? MATRIX_WIDTH - numWidth : (MATRIX_WIDTH - numWidth) / 2;
  memset(numMask, 0, sizeof(numMask));
  textToMask(DOOR_TEXT, numLeft, numMask);
}

// A szám mögötti sávban (és 1-1 oszloppal körülötte) az eső ennyire halványabb,
// hogy a szám jól olvasható maradjon. 1.0 = ugyanolyan fényes, 0 = nincs eső a szám mögött.
const float NUMBER_AREA_DIM = 0.40f;

bool inNumberArea(int x) {
  return x >= numLeft - 1 && x <= numLeft + numWidth;
}

// A (x, y) pixel része-e a "605"-nek
bool isNumberPixel(int x, int y) {
  return numMask[x][y];
}

// A szám szivárványos színe egy adott pixelen (közös minden kiírásnál).
// A színe lassan körbejár, de a fényereje állandó (nem pulzál).
void numberColor(unsigned long now, int x, int y, uint8_t& r, uint8_t& g, uint8_t& b) {
  float baseHue = (now % HUE_PERIOD_MS) / (float)HUE_PERIOD_MS;
  // enyhe átmenet a számokon belül is: balról jobbra és fentről le csúszik a szín
  hsv(baseHue + (x - numLeft) * 0.012f + y * 0.008f, 1.0f, 1.0f, r, g, b);
}

// level: fényerő-szorzó 0..1 (a "zárlatos" villogáshoz)
void drawDoorNumber(unsigned long now, float level) {
  uint8_t r, g, b;
  for (int x = 0; x < MATRIX_WIDTH; x++) {
    for (int y = 0; y < MATRIX_HEIGHT; y++) {
      if (numMask[x][y]) {
        numberColor(now, x, y, r, g, b);
        pxf(x, y, r * level, g * level, b * level);
      }
    }
  }
}

// ------------------------------------------------------------
// "ZÁRLAT": a 605 néha villogva eltűnik, pár pillanatra hibás számot
// mutat, majd villogva visszajön.
// ------------------------------------------------------------

// A hibás szöveg ugyanoda kerül, ahol a 605 áll (a közepéhez igazítva, a kijelzőn belül)
void drawGlitchText(unsigned long now, const char* s, float level) {
  static bool mask[MATRIX_WIDTH][MATRIX_HEIGHT];
  int w = bigTextWidth(s);
  int x = numLeft + numWidth / 2 - w / 2;
  if (x + w > MATRIX_WIDTH) x = MATRIX_WIDTH - w;
  if (x < 0) x = 0;
  memset(mask, 0, sizeof(mask));
  textToMask(s, x, mask);
  uint8_t r, g, b;
  for (int px_ = 0; px_ < MATRIX_WIDTH; px_++) {
    for (int y = 0; y < MATRIX_HEIGHT; y++) {
      if (!mask[px_][y]) continue;
      numberColor(now, px_, y, r, g, b);
      pxf(px_, y, r * level, g * level, b * level);
    }
  }
}

const char* const GLITCH_TEXTS[] = { "DCV", "121x5" };
const int GLITCH_TEXT_COUNT = sizeof(GLITCH_TEXTS) / sizeof(GLITCH_TEXTS[0]);
const unsigned long GLITCH_MIN_GAP = 8000;    // két "zárlat" között legalább ennyi idő telik el (ms)
const unsigned long GLITCH_MAX_GAP = 20000;   // ... és legfeljebb ennyi
const uint16_t GLITCH_OUT_MS = 500;           // villogva eltűnik
const uint16_t GLITCH_ALT_MS = 1300;          // a hibás szám ennyi ideig látszik
const uint16_t GLITCH_IN_MS  = 350;           // villogva visszajön (rövid)

GlitchPhase glitchPhase = GL_NONE;
unsigned long glitchPhaseStart = 0, nextGlitchAt = 0;
int glitchText = 0;

void scheduleGlitch(unsigned long now) {
  glitchPhase = GL_NONE;
  nextGlitchAt = now + random(GLITCH_MIN_GAP, GLITCH_MAX_GAP + 1);
}

void setGlitchPhase(GlitchPhase p, unsigned long now) {
  glitchPhase = p;
  glitchPhaseStart = now;
}

// Azonnal elindít egy "zárlatot" (magától, vagy mozgásra)
void startGlitchNow(unsigned long now) {
  glitchText = random(GLITCH_TEXT_COUNT);
  setGlitchPhase(GL_OUT, now);
  logLine("Glitch: %s", GLITCH_TEXTS[glitchText]);
}

// Az alapkép szám-rajzolása, a "zárlattal" együtt
void drawIdleNumber(unsigned long now) {
  unsigned long t = now - glitchPhaseStart;
  switch (glitchPhase) {
    case GL_NONE:
      if ((long)(now - nextGlitchAt) >= 0) startGlitchNow(now);
      drawDoorNumber(now, 1.0f);
      break;

    case GL_OUT: {
      // egyre ritkábban és bizonytalanabb fénnyel villan fel, aztán kialszik
      float p = 1.0f - t / (float)GLITCH_OUT_MS;
      if (random(100) < p * 85) drawDoorNumber(now, random(35, 101) / 100.0f);
      if (t >= GLITCH_OUT_MS) setGlitchPhase(GL_ALT, now);
      break;
    }

    case GL_ALT:
      // a hibás szám, néha egy-egy kimaradó képkockával
      if (random(100) >= 8) drawGlitchText(now, GLITCH_TEXTS[glitchText], random(70, 101) / 100.0f);
      if (t >= GLITCH_ALT_MS) setGlitchPhase(GL_IN, now);
      break;

    case GL_IN: {
      // gyorsan visszavillog: egyre gyakrabban a 605, közben még be-bevillan a hibás
      float p = t / (float)GLITCH_IN_MS;
      int roll = random(100);
      if (roll < p * 100)             drawDoorNumber(now, 1.0f);
      else if (roll < p * 100 + 20)   drawGlitchText(now, GLITCH_TEXTS[glitchText], 0.7f);
      if (t >= GLITCH_IN_MS) scheduleGlitch(now);
      break;
    }
  }
}

// ============================================================
// MÁTRIX ESŐ - négy karakterméretben
//
// Rétegek, hátulról előre:
//   1. halvány zöld derengés az egész esős sávon (BG_GLOW)
//   2. halvány, lassan váltakozó háttérkarakterek (távoli, elmosódott jelek)
//   3. a lefolyó karakterláncok: fényes fej, egyre halványabb csóva, mögöttük rövid utánvilágítással
// ============================================================

const RainSize RAIN_SIZES[] = {
  //  n  vp hp  len    speed   decay
  {  1, 1, 1,  3, 7,   8, 24,  0.86f },   // 1x1: pixeles eső, minden oszlopban
  {  4, 5, 5,  2, 3,   4, 11,  0.90f },   // 4x4: katakana-szerű jelek
};
const int RAIN_SIZE_COUNT = sizeof(RAIN_SIZES) / sizeof(RAIN_SIZES[0]);
int sizeIndex = 0;                     // 1x1-gyel kezd

const float RAIN_DIM        = 0.35f;   // a háttér-csíkok fényereje az előtérhez képest
const float RAIN_DIM_SPEED  = 0.70f;   // a háttér-csíkok sebessége az előtérhez képest
const float BG_GLOW         = 0.04f;   // az egyenletes zöld derengés erőssége
const float BG_GLYPH_MIN    = 0.06f;   // a háttérkarakterek fényereje (min..max)
const float BG_GLYPH_MAX    = 0.15f;
const int   BG_CHANGE_PER_1000 = 15;   // ennyi ezrelék eséllyel cserélődik egy háttérkarakter képkockánként
const int   HEAD_CHANGE_PCT = 5;       // a fej-karakter cseréjének esélye képkockánként (%)
const int   TAIL_CHANGE_PCT = 2;       // a csóva karaktereinek cseréje (%)

// 2x2-es jelek (bit3 = bal felső, bit2 = jobb felső, bit1 = bal alsó, bit0 = jobb alsó)
const uint8_t GLYPH2[] = { 0b1011, 0b0111, 0b1101, 0b1110, 0b1001, 0b0110, 0b1100, 0b1010, 0b0101, 0b0011 };
const int GLYPH2_COUNT = sizeof(GLYPH2);

// 3x3-as katakana-szerű jelek (bit2 = bal szélső oszlop)
const uint8_t KATA3[][3] = {
  { 0b111, 0b011, 0b010 },  // ア
  { 0b001, 0b110, 0b010 },  // イ
  { 0b010, 0b111, 0b001 },  // ウ
  { 0b111, 0b010, 0b111 },  // エ
  { 0b010, 0b111, 0b110 },  // オ
  { 0b010, 0b111, 0b101 },  // カ
  { 0b111, 0b111, 0b010 },  // キ
  { 0b110, 0b011, 0b010 },  // ク
  { 0b111, 0b001, 0b111 },  // コ
  { 0b101, 0b111, 0b001 },  // サ
  { 0b101, 0b001, 0b110 },  // ソ
  { 0b100, 0b110, 0b100 },  // ト
  { 0b111, 0b000, 0b111 },  // ニ
  { 0b111, 0b011, 0b111 },  // ヨ
  { 0b101, 0b101, 0b001 },  // リ
  { 0b111, 0b101, 0b111 },  // ロ
};
const int KATA3_COUNT = sizeof(KATA3) / sizeof(KATA3[0]);

// 4x4-es katakana-szerű jelek (bit3 = bal szélső oszlop)
const uint8_t KATA4[][4] = {
  { 0b1111, 0b0011, 0b0010, 0b0100 },  // ア
  { 0b0001, 0b0110, 0b1010, 0b0010 },  // イ
  { 0b0100, 0b1111, 0b0001, 0b0110 },  // ウ
  { 0b1111, 0b0010, 0b0010, 0b1111 },  // エ
  { 0b0010, 0b1111, 0b0110, 0b1010 },  // オ
  { 0b0100, 0b1111, 0b0101, 0b1011 },  // カ
  { 0b1111, 0b0100, 0b1111, 0b0100 },  // キ
  { 0b0100, 0b0111, 0b1001, 0b0010 },  // ク
  { 0b1111, 0b0001, 0b0001, 0b1111 },  // コ
  { 0b0101, 0b1111, 0b0101, 0b0010 },  // サ
  { 0b1001, 0b1001, 0b0001, 0b0110 },  // ソ
  { 0b1000, 0b1110, 0b1000, 0b1000 },  // ト
  { 0b1110, 0b0000, 0b0000, 0b1111 },  // ニ
  { 0b1111, 0b0111, 0b0001, 0b1111 },  // ヨ
  { 0b1001, 0b1001, 0b0001, 0b0110 },  // リ
  { 0b1111, 0b1001, 0b1001, 0b1111 },  // ロ
  { 0b1110, 0b0000, 0b1111, 0b0010 },  // テ
  { 0b1111, 0b0111, 0b0010, 0b0100 },  // ヲ
};
const int KATA4_COUNT = sizeof(KATA4) / sizeof(KATA4[0]);

int glyphCount(uint8_t n) {
  switch (n) {
    case 1:  return 1;
    case 2:  return GLYPH2_COUNT;
    case 3:  return KATA3_COUNT;
    default: return KATA4_COUNT;
  }
}

bool glyphPixel(uint8_t n, uint8_t id, int r, int c) {
  switch (n) {
    case 1:  return true;
    case 2:  return (GLYPH2[id] & (0b1000 >> (r * 2 + c))) != 0;
    case 3:  return (KATA3[id][r] & (0b100 >> c)) != 0;
    default: return (KATA4[id][r] & (0b1000 >> c)) != 0;
  }
}

uint8_t randomGlyph() { return random(glyphCount(RAIN_SIZES[sizeIndex].n)); }

// --- az aktuális esőmező ---
int8_t fieldX[MAX_COLS];               // a karakteroszlopok bal széle
bool   fieldBright[MAX_COLS];          // előtér (fényes) vagy háttér-csík
uint8_t fieldCount = 0;
float  fieldSpeedMul = 1.0f;           // a dekódolásnál gyorsabb
GlyphStream streams[MAX_COLS];
uint8_t bgGlyph[MAX_COLS][MAX_CELL_ROWS];   // háttérkarakterek
float   bgLvl[MAX_COLS][MAX_CELL_ROWS];

float ink[MATRIX_WIDTH][MATRIX_HEIGHT];       // a láncok ebben a képkockában
float trail[MATRIX_WIDTH][MATRIX_HEIGHT];     // utánvilágítás
bool  headMask[MATRIX_WIDTH][MATRIX_HEIGHT];  // fehér fej-pixel
float bgBuf[MATRIX_WIDTH][MATRIX_HEIGHT];     // háttérkarakterek

void resetBgCell(int i, int j) {
  bgGlyph[i][j] = randomGlyph();
  // 1x1-nél a háttér csak szórt pixelekből áll, ezért a fele üres marad
  if (RAIN_SIZES[sizeIndex].n == 1 && random(2) == 0) bgLvl[i][j] = 0;
  else bgLvl[i][j] = random((int)(BG_GLYPH_MIN * 100), (int)(BG_GLYPH_MAX * 100) + 1) / 100.0f;
}

void resetStream(int i, bool initial) {
  const RainSize& S = RAIN_SIZES[sizeIndex];
  GlyphStream& s = streams[i];
  bool bright = fieldBright[i];
  s.speed = random(S.spMin, S.spMax + 1) / 100.0f * fieldSpeedMul;
  if (!bright) s.speed *= RAIN_DIM_SPEED;
  s.len = random(S.minLen, S.maxLen + 1);
  for (int k = 0; k < STREAM_MAX_LEN; k++) s.glyph[k] = randomGlyph();
  s.dim = bright ? 1.0f : RAIN_DIM;
  s.whiteHead = bright;
  int span = s.len * S.vpitch;
  s.y = initial ? random(-span, MATRIX_HEIGHT) : -(float)S.n - random(0, 3);
  s.active = true;
  s.restartAt = 0;
}

void addColumn(int x0, bool bright) {
  if (fieldCount >= MAX_COLS) return;
  fieldX[fieldCount] = x0;
  fieldBright[fieldCount] = bright;
  fieldCount++;
}

void buildField(float speedMul) {
  const RainSize& S = RAIN_SIZES[sizeIndex];
  fieldCount = 0;
  fieldSpeedMul = speedMul;
  int count = (MATRIX_WIDTH - S.n) / S.hpitch + 1;
  int span = (count - 1) * S.hpitch + S.n;
  int offset = (MATRIX_WIDTH - span) / 2;
  for (int i = 0; i < count; i++) {
    int fromEdge = min(i, count - 1 - i);
    addColumn(offset + i * S.hpitch, (fromEdge % 2) == 0);
  }
  for (int i = 0; i < fieldCount; i++) {
    resetStream(i, true);
    for (int j = 0; j < MAX_CELL_ROWS; j++) resetBgCell(i, j);
  }
  memset(trail, 0, sizeof(trail));
}

// Az eső "erőssége" 0..1: 1 = teljes eső, 0 = elállt. Ezzel halványul a derengés
// és a háttérkarakterek, és ennyi eséllyel indul új lánc, ha egy oszlop felszabadul.
float rainIntensity = 1.0f;
// A lefolyó csíkok fényereje 0..1 (eláláskor ez is lecseng, hogy ne kelljen megvárni,
// amíg a lassú csíkok kifolynak a kijelzőről)
float rainStreamLevel = 1.0f;

// Egy képkockányi mozgás és a pufferek frissítése.
// maxDelay: legfeljebb ennyi ms szünet az újraindulásig.
void stepRain(unsigned long now, unsigned long maxDelay) {
  const RainSize& S = RAIN_SIZES[sizeIndex];
  memset(ink, 0, sizeof(ink));
  memset(headMask, 0, sizeof(headMask));
  memset(bgBuf, 0, sizeof(bgBuf));

  for (int i = 0; i < fieldCount; i++) {
    GlyphStream& s = streams[i];
    int x0 = fieldX[i];

    // --- mozgás ---
    if (!s.active) {
      if (rainIntensity > 0 && (long)(now - s.restartAt) >= 0) {
        // ahogy áll el az eső, egyre kisebb eséllyel indul új lánc
        if (random(1000) < rainIntensity * 1000) resetStream(i, false);
        else s.restartAt = now + random(300, 1500);
      }
    } else {
      s.y += s.speed;
      if (random(100) < HEAD_CHANGE_PCT) s.glyph[0] = randomGlyph();
      if (s.len > 1 && random(100) < TAIL_CHANGE_PCT) s.glyph[random(1, s.len)] = randomGlyph();
      int tailTop = (int)floorf(s.y) - (s.len - 1) * S.vpitch;
      if (tailTop >= MATRIX_HEIGHT) {
        s.active = false;
        s.restartAt = now + random(0, maxDelay + 1);
      }
    }

    // --- a lánc kirajzolása a pufferbe ---
    if (s.active) {
      int headTop = (int)floorf(s.y);
      for (int k = 0; k < s.len; k++) {
        int top = headTop - k * S.vpitch;
        if (top + S.n <= 0 || top >= MATRIX_HEIGHT) continue;
        float v;
        if (k == 0)          v = 1.0f;
        else if (S.n == 1)   v = 0.85f * powf(0.78f, k - 1);   // pixeles esőnél hosszabb, puhább csóva
        else                 v = 0.80f * powf(0.62f, k - 1);
        v *= s.dim;
        for (int r = 0; r < S.n; r++) {
          int y = top + r;
          if (y < 0 || y >= MATRIX_HEIGHT) continue;
          for (int c = 0; c < S.n; c++) {
            if (!glyphPixel(S.n, s.glyph[k], r, c)) continue;
            int x = x0 + c;
            if (x < 0 || x >= MATRIX_WIDTH) continue;
            if (v > ink[x][y]) {
              ink[x][y] = v;
              headMask[x][y] = (k == 0 && s.whiteHead);
            }
          }
        }
      }
    }

    // --- háttérkarakterek: álló, halvány jelek, lassan cserélődnek ---
    for (int j = 0; j < MAX_CELL_ROWS; j++) {
      int top = j * S.vpitch;
      if (top >= MATRIX_HEIGHT) break;
      if (random(1000) < BG_CHANGE_PER_1000) resetBgCell(i, j);
      if (bgLvl[i][j] <= 0) continue;
      for (int r = 0; r < S.n; r++) {
        int y = top + r;
        if (y >= MATRIX_HEIGHT) break;
        for (int c = 0; c < S.n; c++) {
          if (glyphPixel(S.n, bgGlyph[i][j], r, c)) bgBuf[x0 + c][y] = bgLvl[i][j];
        }
      }
    }
  }

  // --- utánvilágítás ---
  for (int x = 0; x < MATRIX_WIDTH; x++)
    for (int y = 0; y < MATRIX_HEIGHT; y++)
      trail[x][y] = max(trail[x][y] * S.trailDecay, ink[x][y]);
}

// fényesség -> szín: sötétzöld -> élénkzöld -> (csak a legfényesebbnél) menta
void rainColor(float v, float& r, float& g, float& b) {
  g = 255.0f * v;
  b = 18.0f * v;
  r = (v > 0.8f) ? (v - 0.8f) * 5.0f * 70.0f : 0.0f;
}

// fade: 0..1 fényerő-szorzó (a szám mögött halványabb)
void drawRainPixel(int x, int y, float fade) {
  if (fade <= 0) return;
  if (headMask[x][y]) {
    float h = fade * rainStreamLevel;     // eláláskor a még úton lévő csíkok is kihalványulnak
    pxf(x, y, 150 * h, 255 * h, 150 * h);
    return;
  }
  float v = max(BG_GLOW * rainIntensity, max(bgBuf[x][y] * rainIntensity, trail[x][y] * rainStreamLevel));
  if (v < 0.01f) return;
  float r, g, b;
  rainColor(v * 0.9f, r, g, b);
  pxf(x, y, r * fade, g * fade, b * fade);
}

// A dekódolás után ugyanaz az eső folyik tovább, csak lelassul az alap tempóra
void slowRainToIdle() {
  for (int i = 0; i < fieldCount; i++) streams[i].speed /= fieldSpeedMul;
  fieldSpeedMul = 1.0f;
}

// Miután a "605" kiíródott, az eső fokozatosan eláll:
// RAIN_STOP_MS alatt egyre ritkábban indul új csík, a zöld derengés és a háttérkarakterek
// elhalványulnak, a még úton lévő csíkok lefolynak. Utána csak a szám látszik.
const unsigned long RAIN_STOP_MS = 1500;
unsigned long rainStopStart = 0;
bool rainDone = false;

void startRainStop() {
  rainStopStart = millis();
  rainDone = false;
}

bool rainStillVisible() {
  for (int i = 0; i < fieldCount; i++) if (streams[i].active) return true;
  for (int x = 0; x < MATRIX_WIDTH; x++)
    for (int y = 0; y < MATRIX_HEIGHT; y++) if (trail[x][y] > 0.01f) return true;
  return false;
}

void updateIdleScene() {
  unsigned long now = millis();
  if (now - lastIdleFrame < IDLE_FRAME_MS) return;
  lastIdleFrame = now;

  matrix.fillScreen(0);

  if (!rainDone) {
    // lineárisan csökken, de a végén négyzetesen, hogy lágyan csengjen le
    float t = clamp01((now - rainStopStart) / (float)RAIN_STOP_MS);
    rainIntensity = (1.0f - t) * (1.0f - t);
    rainStreamLevel = 1.0f - t * t;          // a csíkok eleinte még látszanak, a végén gyorsan kihunynak
    stepRain(now, 1500);
    for (int x = 0; x < MATRIX_WIDTH; x++) {
      float fade = inNumberArea(x) ? NUMBER_AREA_DIM : 1.0f;
      for (int y = 0; y < MATRIX_HEIGHT; y++) drawRainPixel(x, y, fade);
    }
    if (t >= 1.0f) {
      rainDone = true;                           // elállt: innentől csak a szám
      logLine("Rain stopped");
    }
  }

  drawIdleNumber(now);
  showFrame();
}

// ============================================================
// DEKÓDOLÁS - a teljes kijelzőn folynak a karakterek
// ============================================================
const uint16_t DEC_LOCK_START = 150;    // ettől "akadnak fenn" a karakterek a számjegyekben (ms)
const uint16_t DEC_LOCK_END   = 700;    // eddigre minden számpixel kirajzolódik
const uint16_t DEC_FADE_START = 800;    // innen színeződik át zöldből szivárványba
const uint16_t DEC_END        = 1100;   // a dekódolás vége
const float    DEC_SPEED_MUL  = 3.0f;   // a dekódolós eső ennyiszer gyorsabb az alapnál

uint16_t decLockedAt[MATRIX_WIDTH][MATRIX_HEIGHT];    // mikor akadt fenn (ms); 0 = még nem
uint16_t decForceAt[MATRIX_WIDTH][MATRIX_HEIGHT];     // legkésőbb ekkor fennakad
unsigned long decStart = 0, decLastFrame = 0;

void startDecode() {
  decStart = millis();
  decLastFrame = 0;
  buildField(DEC_SPEED_MUL);
  const RainSize& S = RAIN_SIZES[sizeIndex];
  for (int i = 0; i < fieldCount; i++) {     // szinte azonnal mindenhol folyni kezd
    streams[i].y = random(-(int)S.n - 4, 3);
  }
  for (int x = 0; x < MATRIX_WIDTH; x++) {
    for (int y = 0; y < MATRIX_HEIGHT; y++) {
      decLockedAt[x][y] = 0;
      decForceAt[x][y] = random(DEC_LOCK_START + 500, DEC_LOCK_END);
    }
  }
}

// true, ha véget ért
bool updateDecode() {
  unsigned long now = millis();
  unsigned long el = now - decStart;
  if (el >= DEC_END) return true;
  if (now - decLastFrame < IDLE_FRAME_MS) return false;
  decLastFrame = now;

  float k = clamp01((el - (float)DEC_FADE_START) / (DEC_END - DEC_FADE_START));   // zöld -> szivárvány

  rainIntensity = 1.0f;                       // a kiírás alatt végig teljes erővel esik
  rainStreamLevel = 1.0f;
  stepRain(now, 300);
  matrix.fillScreen(0);

  for (int x = 0; x < MATRIX_WIDTH; x++) {
    // a szám mögött az eső a végére fokozatosan elhalványul az alapkép szintjére
    float outFade = inNumberArea(x) ? lerpf(1.0f, NUMBER_AREA_DIM, k) : 1.0f;
    for (int y = 0; y < MATRIX_HEIGHT; y++) {
      bool num = isNumberPixel(x, y);
      if (num && decLockedAt[x][y] == 0) {
        // egy fényes karakter áthalad a számjegy pixelén: ott fennakad
        bool hit = el >= DEC_LOCK_START && ink[x][y] >= 0.5f;
        if (hit || el >= decForceAt[x][y]) decLockedAt[x][y] = el;
      }

      if (num && decLockedAt[x][y] != 0) {
        // fennakadt pixel: felvillan, majd remegő zöld, végül szivárványba úszik
        unsigned long since = el - decLockedAt[x][y];
        float gr, gg, gb;
        if (since < 160) {
          gr = 170; gg = 255; gb = 170;
        } else {
          float flicker = 1.0f - (1.0f - k) * 0.25f * (random(100) / 100.0f);
          gr = 15 * flicker; gg = 235 * flicker; gb = 35 * flicker;
        }
        uint8_t r, g, b;
        numberColor(now, x, y, r, g, b);
        pxf(x, y, lerpf(gr, r, k), lerpf(gg, g, k), lerpf(gb, b, k));
        continue;
      }
      drawRainPixel(x, y, outFade);
    }
  }
  showFrame();
  return false;
}

// ============================================================
// "Szia! :)" - mozgásra, villogó kurzorral begépelve
// ============================================================
// 7 sor magas kis betűk (a legfelső bit a bal szélső oszlop)
const uint8_t SM_S[7]     = { 0b0110, 0b1001, 0b1000, 0b0110, 0b0001, 0b1001, 0b0110 };
const uint8_t SM_Z[7]     = { 0, 0, 0b111, 0b001, 0b010, 0b100, 0b111 };
const uint8_t SM_I[7]     = { 1, 0, 1, 1, 1, 1, 1 };
const uint8_t SM_A[7]     = { 0, 0, 0b0110, 0b0001, 0b0111, 0b1001, 0b0111 };
const uint8_t SM_EXCL[7]  = { 1, 1, 1, 1, 1, 0, 1 };
const uint8_t SM_SPACE[7] = { 0, 0, 0, 0, 0, 0, 0 };
const uint8_t SM_COLON[7] = { 0, 0, 1, 0, 0, 1, 0 };
const uint8_t SM_PAREN[7] = { 0b10, 0b01, 0b01, 0b01, 0b01, 0b01, 0b10 };

const uint8_t* smallGlyph(char ch, uint8_t& w) {
  switch (ch) {
    case 'S': w = 4; return SM_S;
    case 'z': w = 3; return SM_Z;
    case 'i': w = 1; return SM_I;
    case 'a': w = 4; return SM_A;
    case '!': w = 1; return SM_EXCL;
    case ' ': w = 2; return SM_SPACE;
    case ':': w = 1; return SM_COLON;
    case ')': w = 2; return SM_PAREN;
    default:  w = 0; return nullptr;
  }
}

const char* const HELLO_TEXT = "Szia! :)";
const uint8_t  HELLO_COLOR[3]  = { 0, 255, 255 };   // türkiz (R, G, B)
const uint8_t  CURSOR_COLOR[3] = { 200, 255, 255 }; // a kurzor
const uint16_t TYPE_START_MS   = 400;   // ennyi ideig villog az üres kurzor, mielőtt gépelni kezd
const uint16_t TYPE_MIN_MS     = 70;    // két leütés között legalább ennyi idő telik el ...
const uint16_t TYPE_MAX_MS     = 150;   // ... és legfeljebb ennyi (emberi, egyenetlen tempó)
const uint16_t TYPE_SPACE_MS   = 160;   // szóköz előtt kicsit hosszabb a szünet
const uint16_t TYPE_END_MS     = 900;   // a kész szöveg után ennyi ideig villog még a kurzor
const uint16_t CURSOR_BLINK_MS = 450;   // a kurzor villogásának fél periódusa
const uint8_t  HELLO_MAX_CHARS = 16;

uint16_t typeAt[HELLO_MAX_CHARS];       // mikor jelenik meg az egyes betű (ms a kezdettől)
uint8_t  typeCount = 0;
unsigned long helloStart = 0, helloLastFrame = 0;

void startHello() {
  helloStart = millis();
  helloLastFrame = 0;
  uint16_t t = TYPE_START_MS;
  typeCount = 0;
  for (const char* s = HELLO_TEXT; *s && typeCount < HELLO_MAX_CHARS; s++) {
    t += (*s == ' ') ? TYPE_SPACE_MS : random(TYPE_MIN_MS, TYPE_MAX_MS + 1);
    typeAt[typeCount++] = t;
  }
}

// true, ha véget ért
bool updateHello() {
  unsigned long now = millis();
  unsigned long el = now - helloStart;
  uint16_t typedEnd = typeCount ? typeAt[typeCount - 1] : 0;
  if (el >= (unsigned long)typedEnd + TYPE_END_MS) return true;
  if (now - helloLastFrame < IDLE_FRAME_MS) return false;
  helloLastFrame = now;

  int w = 0;
  for (const char* s = HELLO_TEXT; *s; s++) {
    uint8_t gw;
    if (!smallGlyph(*s, gw)) continue;
    if (w) w += 1;
    w += gw;
  }
  int x = (MATRIX_WIDTH - w) / 2;
  int cursorX = x;

  matrix.fillScreen(0);
  uint8_t i = 0;
  for (const char* s = HELLO_TEXT; *s && i < typeCount; s++, i++) {
    if (el < typeAt[i]) break;                  // ezt a betűt még nem "ütötték le"
    uint8_t gw;
    const uint8_t* rows = smallGlyph(*s, gw);
    if (!rows) continue;
    bool fresh = (el - typeAt[i]) < 60;         // a frissen leütött betű egy pillanatra fényesebb
    for (int r = 0; r < 7; r++) {
      for (int c = 0; c < gw; c++) {
        if (!(rows[r] & (1 << (gw - 1 - c)))) continue;
        if (fresh) px(x + c, r + 1, 120, 255, 255);
        else       px(x + c, r + 1, HELLO_COLOR[0], HELLO_COLOR[1], HELLO_COLOR[2]);
      }
    }
    x += gw + 1;
    cursorX = x;
  }

  // gépelés közben a kurzor folyamatosan látszik, előtte és utána villog
  bool typing = el >= TYPE_START_MS && el < (unsigned long)typedEnd + 150;
  bool cursorOn = typing || ((el / CURSOR_BLINK_MS) % 2 == 0);
  if (cursorOn && cursorX < MATRIX_WIDTH) {
    for (int r = 1; r < MATRIX_HEIGHT; r++) px(cursorX, r, CURSOR_COLOR[0], CURSOR_COLOR[1], CURSOR_COLOR[2]);
  }
  showFrame();
  return false;
}

// ============================================================
// FUTÓ SZÖVEG
// ============================================================
// 8 sor magas kis betűk (a 6. sor az alapvonal, a "g" szára a 7. sorba lóg)
const uint8_t SC_H[8]     = { 0b1001, 0b1001, 0b1001, 0b1111, 0b1001, 0b1001, 0b1001, 0 };
const uint8_t SC_I[8]     = { 1, 0, 1, 1, 1, 1, 1, 0 };
const uint8_t SC_G[8]     = { 0b0111, 0b1000, 0b1000, 0b1011, 0b1001, 0b1001, 0b0111, 0 };
const uint8_t SC_E[8]     = { 0, 0, 0b0110, 0b1001, 0b1111, 0b1000, 0b0111, 0 };
const uint8_t SC_O[8]     = { 0, 0, 0b0110, 0b1001, 0b1001, 0b1001, 0b0110, 0 };
const uint8_t SC_R[8]     = { 0, 0, 0b101, 0b110, 0b100, 0b100, 0b100, 0 };
const uint8_t SC_GL[8]    = { 0, 0, 0b0111, 0b1001, 0b1001, 0b0111, 0b0001, 0b0110 };
const uint8_t SC_EXCL[8]  = { 1, 1, 1, 1, 1, 0, 1, 0 };
const uint8_t SC_SPACE[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };

const uint8_t* scrollGlyph(char ch, uint8_t& w) {
  switch (ch) {
    case 'H': w = 4; return SC_H;
    case 'i': w = 1; return SC_I;
    case 'G': w = 4; return SC_G;
    case 'e': w = 4; return SC_E;
    case 'o': w = 4; return SC_O;
    case 'r': w = 3; return SC_R;
    case 'g': w = 4; return SC_GL;
    case '!': w = 1; return SC_EXCL;
    case ' ': w = 3; return SC_SPACE;
    default:  w = 0; return nullptr;
  }
}

const uint8_t SCROLL_COLOR[3] = { 255, 0, 0 };
const float    SCROLL_SPEED   = 0.024f;           // pixel / ms (kb. 24 pixel másodpercenként)
const char*    scrollText = "";
int            scrollWidth = 0;
unsigned long  scrollStart = 0, scrollLastFrame = 0;

void startScroll(const char* text) {
  scrollText = text;
  scrollWidth = 0;
  for (const char* s = text; *s; s++) {
    uint8_t gw;
    if (!scrollGlyph(*s, gw)) continue;
    if (scrollWidth) scrollWidth += 1;
    scrollWidth += gw;
  }
  scrollStart = millis();
  scrollLastFrame = 0;
}

// true, ha véget ért
bool updateScroll() {
  unsigned long now = millis();
  unsigned long el = now - scrollStart;
  int x = MATRIX_WIDTH - (int)(el * SCROLL_SPEED);
  if (x < -scrollWidth) return true;
  if (now - scrollLastFrame < IDLE_FRAME_MS) return false;
  scrollLastFrame = now;

  // pislákolás
  float level = (random(100) < 12) ? random(25, 70) / 100.0f : 1.0f;

  matrix.fillScreen(0);
  for (const char* s = scrollText; *s; s++) {
    uint8_t gw;
    const uint8_t* rows = scrollGlyph(*s, gw);
    if (!rows) continue;
    for (int r = 0; r < MATRIX_HEIGHT; r++)
      for (int c = 0; c < gw; c++)
        if ((rows[r] & (1 << (gw - 1 - c))) && x + c >= 0 && x + c < MATRIX_WIDTH)
          pxf(x + c, r, SCROLL_COLOR[0] * level, SCROLL_COLOR[1] * level, SCROLL_COLOR[2] * level);
    x += gw + 1;
  }
  showFrame();
  return false;
}

// ============================================================
// BECSAPÓDÓ "605"
// ============================================================
const uint16_t SLAM_GAP_MS    = 330;   // ennyi idő telik el két jegy indulása között
const uint16_t SLAM_TRAVEL_MS = 260;   // ennyi ideig repül egy jegy
const uint16_t SLAM_FLASH_MS  = 180;   // a becsapódás fehér villanása
const uint16_t SLAM_SHAKE_MS  = 70;    // a már a helyükön lévő jegyek ennyi ideig rándulnak meg
const uint16_t SLAM_HOLD_MS   = 450;   // az utolsó becsapódás után ennyi idő múlva vált az alapképre
#define SLAM_MAX_CHARS 8
#define SLAM_SPARKS    24

SlamChar slamChars[SLAM_MAX_CHARS];
uint8_t slamCount = 0;
float sparkX[SLAM_SPARKS], sparkY[SLAM_SPARKS], sparkVX[SLAM_SPARKS], sparkVY[SLAM_SPARKS], sparkLife[SLAM_SPARKS];
unsigned long slamStart = 0, slamLastFrame = 0;

void startSlam() {
  slamStart = millis();
  slamLastFrame = 0;
  slamCount = 0;
  int x = numLeft;
  for (const char* s = DOOR_TEXT; *s && slamCount < SLAM_MAX_CHARS; s++) {
    uint8_t gw;
    const uint8_t* rows = bigGlyph(*s, gw);
    if (!rows) continue;
    slamChars[slamCount++] = { rows, gw, (int8_t)x, false };
    x += gw + 1;
  }
  for (int i = 0; i < SLAM_SPARKS; i++) sparkLife[i] = 0;
}

void spawnSparks(int x) {
  int made = 0;
  for (int i = 0; i < SLAM_SPARKS && made < 6; i++) {
    if (sparkLife[i] > 0) continue;
    sparkX[i] = x; sparkY[i] = random(0, 8);
    sparkVX[i] = random(30, 90) / 100.0f;
    sparkVY[i] = random(-40, 41) / 100.0f;
    sparkLife[i] = 1.0f;
    made++;
  }
}

void drawSlamChar(unsigned long now, const SlamChar& ch, int x, float level, float flash) {
  uint8_t r, g, b;
  for (int y = 0; y < MATRIX_HEIGHT; y++) {
    for (int c = 0; c < ch.w; c++) {
      if (!(ch.rows[y] & (1 << (ch.w - 1 - c)))) continue;
      int X = x + c;
      if (X < 0 || X >= MATRIX_WIDTH) continue;
      numberColor(now, ch.x + c, y, r, g, b);
      pxf(X, y, lerpf(r, 255, flash) * level, lerpf(g, 255, flash) * level, lerpf(b, 255, flash) * level);
    }
  }
}

// true, ha véget ért
bool updateSlam() {
  unsigned long now = millis();
  unsigned long el = now - slamStart;
  unsigned long total = (unsigned long)(slamCount - 1) * SLAM_GAP_MS + SLAM_TRAVEL_MS + SLAM_HOLD_MS;
  if (el >= total) return true;
  if (now - slamLastFrame < 30) return false;
  slamLastFrame = now;

  matrix.fillScreen(0);

  // rándulás: ha épp most csapódott be egy jegy, a már a helyükön lévők 1 px-t ugranak
  int shake = 0;
  for (int k = 0; k < slamCount; k++) {
    long lt = (long)el - (long)k * SLAM_GAP_MS - SLAM_TRAVEL_MS;
    if (lt >= 0 && lt < SLAM_SHAKE_MS) shake = 1;
  }

  // jobbról balra érkeznek: először az 5, végül a 6
  for (int k = 0; k < slamCount; k++) {
    SlamChar& ch = slamChars[slamCount - 1 - k];
    long st = (long)el - (long)k * SLAM_GAP_MS;
    if (st < 0) continue;
    if (st < SLAM_TRAVEL_MS) {
      float p = st / (float)SLAM_TRAVEL_MS;
      float x = lerpf(-ch.w - 1, ch.x, p * p * p);        // gyorsulva repül
      int ix = (int)roundf(x);
      if (p > 0.3f) {                                      // elmosódó csík mögötte
        drawSlamChar(now, ch, ix - 4, 0.15f * p, 0);
        drawSlamChar(now, ch, ix - 2, 0.35f * p, 0);
      }
      drawSlamChar(now, ch, ix, 1.0f, 0);
    } else {
      long lt = st - SLAM_TRAVEL_MS;
      if (!ch.landed) { ch.landed = true; spawnSparks(ch.x + ch.w); }
      float flash = clamp01(1.0f - lt / (float)SLAM_FLASH_MS);
      int sh = (lt >= SLAM_SHAKE_MS) ? shake : 0;          // a most érkező nem rándul, a többi igen
      drawSlamChar(now, ch, ch.x + sh, 1.0f, flash);
    }
  }

  // szikrák
  for (int i = 0; i < SLAM_SPARKS; i++) {
    if (sparkLife[i] <= 0) continue;
    sparkX[i] += sparkVX[i]; sparkY[i] += sparkVY[i]; sparkVY[i] += 0.08f;
    sparkLife[i] -= 0.09f;
    int X = (int)roundf(sparkX[i]), Y = (int)roundf(sparkY[i]);
    if (sparkLife[i] > 0 && X >= 0 && X < MATRIX_WIDTH && Y >= 0 && Y < MATRIX_HEIGHT) {
      float a = sparkLife[i] * sparkLife[i];
      pxf(X, Y, 255 * a, 230 * a, 150 * a);
    }
  }
  showFrame();
  return false;
}

// ============================================================
// SZEM ANIMÁCIÓ - mozgásra
// ============================================================
const EyeStyle EYE_DEMON = { "demon", {255, 230,  60}, {255,  60,   0}, { 70,  0,  0}, 0.9f,  false, false, 0, 0 };
const EyeStyle EYE_CAT   = { "cat",   {240, 255,  90}, {110, 210,   0}, {  0, 40,  0}, 0.25f, true, false, 0, 0 };
const EyeStyle EYE_BLUE  = { "blue",  {210, 245, 255}, {  0, 110, 255}, {  0,  5, 70}, 0.0f,  false, false, 0, 0 };
// Emberi kék szem: szürke átmenetes szemfehérje, kék írisz, kis pupilla
const EyeStyle EYE_HUMAN = { "human", {170, 235, 255}, {  0, 115, 255}, {  0, 15, 80}, 0.0f,  false,
                             true, 3.0f, 1.0f };
const float SCLERA_LEVEL = 0.45f;   // a szemfehérje fényereje 0..1 (az előnézet csúszkája: 45)

// (Hogy melyik szem mikor jön, azt a Vezérlés részben a GAGS lista mondja meg.

const float EYE_CX[2] = { 7.5f, 23.5f };
const float EYE_CY    = 3.5f;
const float EYE_A     = 6.8f;   // a szem fél szélessége
const float EYE_B     = 4.3f;   // a szem fél magassága
const float IRIS_R    = 5.5f;   // ekkora sugaron fut ki a színátmenet
const uint16_t EYE_FRAME_MS = 30;
const uint16_t EYE_DURATION_MS = 3000;

const EyeStyle* eyeStyle = &EYE_DEMON;
unsigned long eyeStart = 0, eyeDuration = 0, eyeLastFrame = 0;
unsigned long eyeNextGaze = 0, eyeNextBlink = 0, eyeBlinkStart = 0;
bool eyeBlinking = false, eyeDoubleBlink = false;
float lookX = 0, lookY = 0, tgtX = 0, tgtY = 0;
float pupil = 1, tgtPupil = 1;
float squint = 0, tgtSquint = 0;

// closure: 0 = nyitva, 1 = teljesen csukva
void drawEyes(float closure) {
  const EyeStyle& s = *eyeStyle;
  matrix.fillScreen(0);

  float cu = clamp01(closure + squint * 0.40f);
  float cl = clamp01(closure + squint * 0.30f);
  float top  = EYE_CY - EYE_B - 0.3f;
  float bot  = EYE_CY + EYE_B + 0.3f;
  float meet = EYE_CY + 0.8f;

  for (int e = 0; e < 2; e++) {
    float cx   = EYE_CX[e];
    float side = (e == 0) ? 1.0f : -1.0f;
    float icx  = cx + lookX * 3.0f;
    float icy  = EYE_CY + lookY * 1.3f;
    int hlx = (int)roundf(icx - 1.3f);
    int hly = (int)roundf(icy - 1.4f);

    for (int x = (int)(cx - EYE_A - 1); x <= (int)(cx + EYE_A + 1); x++) {
      if (x < 0 || x >= MATRIX_WIDTH) continue;
      for (int y = 0; y < MATRIX_HEIGHT; y++) {
        float nx = (x - cx) / EYE_A, ny = (y - EYE_CY) / EYE_B;
        float en = sqrtf(nx * nx + ny * ny);
        float shape = clamp01((1.0f - en) * EYE_B + 0.5f);
        if (shape <= 0) continue;

        float slant = s.anger * 0.45f * (x - cx) * side * (1 - closure);
        float yU = lerpf(top, meet, cu) + fabsf(s.anger) * 0.9f * (1 - closure) + slant + lookY * 0.7f;
        float yL = lerpf(bot, meet, cl);
        float a = shape * clamp01(y - yU + 0.5f) * clamp01(yL - y + 0.5f);
        if (a <= 0.01f) continue;

        float dx = x - icx, dy = y - icy;
        float d = sqrtf(dx * dx + dy * dy);
        float r, g, b;

        if (s.sclera) {
          // --- emberi szem ---
          float IR = s.irisR, pr = s.pupilR * pupil;
          float t = clamp01((d - pr) / (IR - pr));
          float ir, ig, ib;
          if (t < 0.35f) {
            float kk = t / 0.35f;
            ir = lerpf(s.core[0], s.mid[0], kk); ig = lerpf(s.core[1], s.mid[1], kk); ib = lerpf(s.core[2], s.mid[2], kk);
          } else {
            float kk = (t - 0.35f) / 0.65f;
            ir = lerpf(s.mid[0], s.edge[0], kk); ig = lerpf(s.mid[1], s.edge[1], kk); ib = lerpf(s.mid[2], s.edge[2], kk);
          }
          float th = atan2f(dy, dx);
          float fib = 0.82f + 0.18f * sinf(th * 9 + 2.5f * sinf(th * 3));   // írisz-rostok
          float q = (d - IR * 0.92f) / 0.45f;
          float ring = expf(-q * q);                                     // sötét perem az írisz szélén
          float m = fib * (1 - 0.6f * ring);
          ir *= m; ig *= m; ib *= m;

          float sv = SCLERA_LEVEL * 255.0f * (1 - 0.6f * en * en);       // szürke átmenet a szemzugok felé
          float k = clamp01(IR - d + 0.5f);                              // írisz vagy szemfehérje
          r = lerpf(sv * 0.93f, ir, k); g = lerpf(sv * 0.95f, ig, k); b = lerpf(sv, ib, k);

          float dark = clamp01(0.5f - (d - pr));                         // pupilla
          float shade = 0.35f + 0.65f * clamp01((y - yU) / 1.8f);        // a felső szemhéj árnyéka
          r *= shade * (1 - dark); g *= shade * (1 - dark); b *= shade * (1 - dark);

          if (x == (int)roundf(icx - 1.0f) && y == (int)roundf(icy - 1.1f)) {   // csillanás
            r = lerpf(r, 255, 0.9f); g = lerpf(g, 255, 0.9f); b = lerpf(b, 255, 0.9f);
          }
          pxf(x, y, r * a, g * a, b * a);
          continue;
        }

        // --- izzó szem (démon, macska, régi kék) ---
        float t = clamp01(d / IRIS_R);
        if (t < 0.35f) {
          float kk = t / 0.35f;
          r = lerpf(s.core[0], s.mid[0], kk); g = lerpf(s.core[1], s.mid[1], kk); b = lerpf(s.core[2], s.mid[2], kk);
        } else {
          float kk = (t - 0.35f) / 0.65f;
          r = lerpf(s.mid[0], s.edge[0], kk); g = lerpf(s.mid[1], s.edge[1], kk); b = lerpf(s.mid[2], s.edge[2], kk);
        }

        float rim = clamp01((1.0f - en) * 2.2f);
        float shadow = clamp01((y - yU) / 2.0f);
        float shade = (0.45f + 0.55f * rim) * (0.5f + 0.5f * shadow);

        float pd;
        if (s.slitPupil) {
          float wx = 0.8f * pupil, hy = 2.8f;
          pd = (sqrtf((dx / wx) * (dx / wx) + (dy / hy) * (dy / hy)) - 1.0f) * wx;
        } else {
          pd = d - 1.6f * pupil;
        }
        float dark = clamp01(0.5f - pd);
        r *= shade * (1 - dark); g *= shade * (1 - dark); b *= shade * (1 - dark);

        if (x == hlx && y == hly) {
          r = lerpf(r, 255, 0.85f); g = lerpf(g, 255, 0.85f); b = lerpf(b, 255, 0.85f);
        }
        pxf(x, y, r * a, g * a, b * a);
      }
    }
  }
  showFrame();
}

void startEyes(uint16_t durationMs, const EyeStyle& style) {
  eyeStyle = &style;
  eyeStart = eyeLastFrame = millis();
  eyeDuration = durationMs;
  lookX = lookY = tgtX = tgtY = 0;
  pupil = tgtPupil = 1;
  squint = tgtSquint = 0;
  eyeBlinking = false;
  eyeNextGaze  = eyeStart + 400;
  eyeNextBlink = eyeStart + random(1500, 3000);
  drawEyes(1.0f);
}

// true, ha véget ért
bool updateEyes() {
  unsigned long now = millis();
  unsigned long el = now - eyeStart;
  if (el >= eyeDuration) return true;
  if (now - eyeLastFrame < EYE_FRAME_MS) return false;
  eyeLastFrame = now;

  if ((long)(now - eyeNextGaze) >= 0) {
    static const float xs[] = { -1.0f, -0.6f, 0.0f, 0.0f, 0.6f, 1.0f };
    tgtX = xs[random(6)];
    tgtY = random(-5, 6) / 10.0f;
    tgtPupil = random(80, 126) / 100.0f;
    tgtSquint = (random(5) == 0) ? 0.6f : 0.0f;
    eyeNextGaze = now + random(500, 1800);
  }
  if (!eyeBlinking && (long)(now - eyeNextBlink) >= 0) {
    eyeBlinking = true;
    eyeBlinkStart = now;
    eyeDoubleBlink = (random(4) == 0);
  }

  lookX  += (tgtX - lookX) * 0.35f;
  lookY  += (tgtY - lookY) * 0.35f;
  pupil  += (tgtPupil - pupil) * 0.15f;
  squint += (tgtSquint - squint) * 0.20f;

  float closure = 0;
  if (eyeBlinking) {
    const unsigned long BL = 180, GAP = 80;
    unsigned long bt = now - eyeBlinkStart;
    unsigned long total = eyeDoubleBlink ? (2 * BL + GAP) : BL;
    if (bt >= total) {
      eyeBlinking = false;
      eyeNextBlink = now + random(1800, 4500);
    } else {
      unsigned long p = bt % (BL + GAP);
      if (p < BL) closure = 1.0f - fabsf(2.0f * p / BL - 1.0f);
    }
  }
  if (el < 350) closure = max(closure, 1.0f - el / 350.0f);          // ébredés
  unsigned long left = eyeDuration - el;
  if (left < 300) closure = max(closure, 1.0f - left / 300.0f);      // elalvás

  drawEyes(closure);
  return false;
}

// ============================================================
// Vezérlés:  kiírás (becsapódás vagy dekódolás) -> alapkép (605) ->
//            mozgásra egy poén a GAGS listából -> (ha kell) a szám új kiírása -> ...
// ============================================================
enum SignState { SIGN_SLAM, SIGN_DECODE, SIGN_IDLE, SIGN_HELLO, SIGN_EYES, SIGN_SCROLL };
SignState signState = SIGN_SLAM;
unsigned long idleSince = 0;
const unsigned long EYES_COOLDOWN_MS = 5000;   // a 605 legalább ennyi ideig látszik, mielőtt újra jöhet egy poén

// --- Poénok mozgásra, ebben a sorrendben körbe ---
#define GAG_EYES   0    // szem animáció (utána, ha van, futó szöveg)
#define GAG_HELLO  1    // "Szia! :)" begépelve
#define GAG_GLITCH 2    // "zárlat": a 605 helyén pár pillanatra hibás szám
struct GagDef {
  uint8_t type;
  const EyeStyle* eye;     // GAG_EYES-nél: melyik szem
  const char* text;        // GAG_EYES-nél: utána futó szöveg (nullptr = nincs)
};
const GagDef GAGS[] = {
  { GAG_EYES,   &EYE_DEMON, "Hi Georgie!" },   // piros szem, utána "Hi Georgie"
  { GAG_GLITCH, nullptr,    nullptr },
  { GAG_EYES,   &EYE_CAT,   nullptr },        // zöld macskaszem
  { GAG_HELLO,  nullptr,    nullptr },        // "Szia! :)"
  { GAG_EYES,   &EYE_HUMAN, nullptr },        // emberi kék szem
  { GAG_GLITCH, nullptr,    nullptr },
};
const int GAG_COUNT = sizeof(GAGS) / sizeof(GAGS[0]);
int gagIndex = 0;
const GagDef* currentGag = nullptr;

// A körforgás lépései: { kiírás módja, karakterméret (RAIN_SIZES index), elrendezés (LAYOUTS index), vastag betű }
// Kiírás módja: REVEAL_SLAM = becsapódó jegyek, REVEAL_RAIN = mátrix esőből dekódolva
#define REVEAL_SLAM 0
#define REVEAL_RAIN 1
const uint8_t CYCLE[][4] = {
  { REVEAL_SLAM, 0, 0, 1 },   // becsapódó vastag 605, középen
  { REVEAL_RAIN, 0, 0, 0 },   // 1x1 eső, középen
  { REVEAL_RAIN, 1, 0, 0 },   // 4x4 eső, jobbra
  { REVEAL_RAIN, 0, 0, 0 },   // 1x1 eső, jobbra
  { REVEAL_RAIN, 1, 0, 0 },   // 4x4 eső, középen
};
const int CYCLE_LEN = sizeof(CYCLE) / sizeof(CYCLE[0]);
int cycleStep = 0;

// Beállítja és elindítja a körforgás aktuális lépésének kiírását
void startCycleStep() {
  sizeIndex   = CYCLE[cycleStep][1];
  layoutIndex = CYCLE[cycleStep][2];
  numberBold  = CYCLE[cycleStep][3] != 0;
  layoutNumber();
  if (CYCLE[cycleStep][0] == REVEAL_SLAM) {
    logLine("Reveal: slam, layout: %s, %s", LAYOUTS[layoutIndex].name, numberBold ? "bold" : "thin");
    startSlam();
    signState = SIGN_SLAM;
  } else {
    uint8_t n = RAIN_SIZES[sizeIndex].n;
    logLine("Reveal: rain %ux%u, layout: %s, %s", n, n, LAYOUTS[layoutIndex].name, numberBold ? "bold" : "thin");
    startDecode();
    signState = SIGN_DECODE;
  }
}

// A poén után a szám újra kiíródik, a körforgás következő módján
void nextReveal() {
  cycleStep = (cycleStep + 1) % CYCLE_LEN;
  startCycleStep();
}

void enterIdle(bool withRain) {
  if (withRain) startRainStop();     // a szám kész: innentől lassan eláll az eső
  else { rainDone = true; fieldCount = 0; }
  lastIdleFrame = 0;
  idleSince = millis();
  scheduleGlitch(idleSince);
  signState = SIGN_IDLE;
}

void setup() {
  Serial.begin(115200);
  pinMode(MOTION_PIN, INPUT_PULLDOWN);

  matrix.begin();
  matrix.setTextWrap(false);
  matrix.setBrightness(MATRIX_BRIGHTNESS);
  matrix.fillScreen(0);
  showFrame();

  logLine("Door sign ready");
  startCycleStep();                  // indításkor az első lépés: a becsapódó 605
}

void loop() {
  unsigned long now = millis();
  bool sensor = (digitalRead(MOTION_PIN) == HIGH);
  bool serialTest = false;
  if (Serial.available()) {          // teszt: bármilyen karakter a soros monitoron = mozgás
    while (Serial.available()) Serial.read();
    serialTest = true;
  }

  switch (signState) {
    case SIGN_SLAM:
      if (updateSlam()) enterIdle(false);
      break;

    case SIGN_DECODE:
      // a csíkok a dekódolás tempójával folynak tovább (nem lassulnak le), így hamarabb kifolynak
      if (updateDecode()) enterIdle(true);
      break;

    case SIGN_IDLE: {
      bool cooledDown = (now - idleSince >= EYES_COOLDOWN_MS);
      bool glitchBusy = (glitchPhase != GL_NONE);          // épp "zárlat" fut: azt nem szakítjuk félbe
      if (((sensor && cooledDown) || serialTest) && !glitchBusy) {
        currentGag = &GAGS[gagIndex];
        gagIndex = (gagIndex + 1) % GAG_COUNT;
        switch (currentGag->type) {
          case GAG_EYES:
            logLine("Motion -> eyes: %s", currentGag->eye->name);
            startEyes(EYE_DURATION_MS, *currentGag->eye);
            signState = SIGN_EYES;
            break;
          case GAG_HELLO:
            logLine("Motion -> hello");
            startHello();
            signState = SIGN_HELLO;
            break;
          case GAG_GLITCH:
            logLine("Motion -> glitch");
            startGlitchNow(now);         // a szám a helyén marad, csak "zárlatos" lesz egy pillanatra
            idleSince = now;
            updateIdleScene();
            break;
        }
      } else {
        updateIdleScene();
      }
      break;
    }

    case SIGN_EYES:
      if (updateEyes()) {
        if (currentGag && currentGag->text) {
          startScroll(currentGag->text);
          signState = SIGN_SCROLL;
        } else {
          nextReveal();
        }
      }
      break;

    case SIGN_SCROLL:
      if (updateScroll()) nextReveal();
      break;

    case SIGN_HELLO:
      if (updateHello()) nextReveal();
      break;
  }
  delay(1);
}
