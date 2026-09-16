#ifdef _WIN32
#include <windows.h>
#endif
#include <GL/glut.h>
#include <cmath>
#include <cstdlib>
#include <ctime>
#include <cstdio>
#include <cstring>

// ---------------------------------------------------------------------------
// Window / world constants
// ---------------------------------------------------------------------------
const int   WIN_W = 1000, WIN_H = 600;   // logical world size (also default window size)
const float PI    = 3.14159265f;

// Vertical bands of the scene (in world coordinates, y grows upward)
const float ROAD_Y0 = 0.0f,   ROAD_Y1 = 110.0f;   // road (cars)
const float SIDE_Y0 = 110.0f, SIDE_Y1 = 140.0f;   // sidewalk (lamp posts, signal pole)
const float RAIL_Y0 = 140.0f, RAIL_Y1 = 170.0f;   // railway track (train)
const float GRASS_Y0= 170.0f, GRASS_Y1= 205.0f;   // grass strip (trees)
const float BUILD_BASE = 205.0f;                  // buildings sit on top of this

// Key x-positions
const float SIGNAL_STOP_X = 300.0f;   // cars must stop here if light is not GREEN
const float GATE_STOP_X   = 650.0f;   // cars (left-lane, moving right) stop here if the gate is down
const float GATE_STOP_X2  = 670.0f;   // cars2 (opposite lane, moving left) stop here if the gate is down
const float GATE_X        = 660.0f;   // physical location of the railway gate arm
const float SIGNAL_POLE_X = 300.0f;

// ---------------------------------------------------------------------------
// Global simulation state
// ---------------------------------------------------------------------------
bool  paused = false;

// ---- Day / Night control (MANUAL - triggered by key 1 / 2, not automatic) --
// dayPhase is only ever changed by an active transition started with the
// '1' (go to night) or '2' (go to day) key. It stays perfectly still
// otherwise, so the scene no longer auto-cycles.
float dayPhase = 0.20f;         // 0..1 ; 0.20 = a nice bright daytime sky
const float PHASE_DAY   = 0.20f; // resting "day" sky position
const float PHASE_NIGHT = 0.78f; // resting "night" sky position

enum SceneState { STATE_DAY, STATE_NIGHT };
SceneState sceneState = STATE_DAY;

bool  transitioning = false;   // true while sun/moon is actively animating
float transStart    = 0.0f;    // dayPhase value at the start of the transition
float transEnd      = 0.0f;    // dayPhase value (possibly >1, wraps) at the end
float transElapsed  = 0.0f;    // seconds since the transition started
float transDuration = 4.0f;    // how many seconds a full sunset/sunrise takes (+/- adjusts this)

// ---- Rain mode (key 3) -------------------------------------------------------
// When active: sun AND moon are both hidden, the sky turns stormy grey,
// dark rain clouds roll in and rain falls. Everything else (traffic,
// railway, cars, buildings) keeps working underneath the rain.
bool rainMode = false;

struct Color { float r, g, b; };

inline Color lerpColor(const Color& a, const Color& b, float t) {
    if (t < 0) t = 0; if (t > 1) t = 1;
    Color c;
    c.r = a.r + (b.r - a.r) * t;
    c.g = a.g + (b.g - a.g) * t;
    c.b = a.b + (b.b - a.b) * t;
    return c;
}

// sky colour keyframes across the day (phase 0..1)
struct SkyKey { float phase; Color c; };
SkyKey skyKeys[] = {
    {0.00f, {1.00f, 0.62f, 0.35f}},  // sunrise glow
    {0.12f, {0.45f, 0.75f, 1.00f}},  // morning blue
    {0.40f, {0.35f, 0.70f, 1.00f}},  // midday blue
    {0.50f, {1.00f, 0.55f, 0.30f}},  // sunset orange
    {0.58f, {0.45f, 0.25f, 0.45f}},  // dusk purple
    {0.66f, {0.06f, 0.07f, 0.20f}},  // night navy
    {0.92f, {0.05f, 0.06f, 0.18f}},  // deep night
    {1.00f, {1.00f, 0.62f, 0.35f}}   // wraps back to sunrise
};
const int SKY_KEY_COUNT = sizeof(skyKeys) / sizeof(SkyKey);

Color getSkyColor(float phase) {
    for (int i = 0; i < SKY_KEY_COUNT - 1; ++i) {
        if (phase >= skyKeys[i].phase && phase <= skyKeys[i + 1].phase) {
            float span = skyKeys[i + 1].phase - skyKeys[i].phase;
            float t = (span > 0.0001f) ? (phase - skyKeys[i].phase) / span : 0.0f;
            return lerpColor(skyKeys[i].c, skyKeys[i + 1].c, t);
        }
    }
    return skyKeys[0].c;
}

// how "dark" it is (0 = full day, 1 = full night) -- drives lights, stars, moon fade
float nightAmount(float phase) {
    Color day  = {0.9f, 0.9f, 1.0f};
    Color cur  = getSkyColor(phase);
    // brightness proxy from luminance vs a reference night colour
    float lum = 0.299f * cur.r + 0.587f * cur.g + 0.114f * cur.b;
    float t = 1.0f - (lum - 0.12f) / (0.85f - 0.12f);
    if (t < 0) t = 0; if (t > 1) t = 1;
    return t;
}

// ---- Traffic signal (MANUAL - key 6 = force RED, key 7 = force GREEN) ------
enum LightState { LS_GREEN, LS_YELLOW, LS_RED };
LightState lightState = LS_GREEN;

// ---- Railway crossing (MANUAL - key 4 = start train, key 5 = stop train) ---
bool trainRunning = false; // train sequence only plays while this is true
float railTimer = 0.0f;
const float CYCLE_LEN      = 24.0f;
const float WARN_START     = 9.0f;
const float GATE_CLOSE_END = 10.6f;
const float TRAIN_START    = 11.2f;
const float TRAIN_END      = 18.5f;
const float GATE_OPEN_START= 18.6f;
const float GATE_OPEN_END  = 20.4f;
float gateAngle = 0.0f; // 0 = open (raised), 90 = closed (barrier down)

// ---- Cars -------------------------------------------------------------------
struct Car {
    float x, y;
    float speed;
    Color body;
};
const int CAR_COUNT = 3;
Car cars[CAR_COUNT];

struct Car2 { float x, y; float speed; Color body; }; // opposite-lane decorative traffic
const int CAR2_COUNT = 2;
Car2 cars2[CAR2_COUNT];

// ---- Buildings ---------------------------------------------------------------
struct Building {
    float x, w, h;
    Color color;
};
const int BUILD_COUNT = 9;
Building buildings[BUILD_COUNT];
bool windowLit[BUILD_COUNT][8][4]; // precomputed random lit windows (max grid)

// ---- Stars --------------------------------------------------------------------
const int STAR_COUNT = 60;
float starX[STAR_COUNT], starY[STAR_COUNT], starTwinkle[STAR_COUNT];

// ---- Clouds ---------------------------------------------------------------------
struct Cloud { float x, y, scale, speed; };
const int CLOUD_COUNT = 4;
Cloud clouds[CLOUD_COUNT];

// ---- Trees -----------------------------------------------------------------------
struct Tree { float x; };
const int TREE_COUNT = 6;
Tree trees[TREE_COUNT];

// ---- Rain drops & storm clouds (used only in Rain mode) ----------------------
struct RainDrop { float x, y, len, speed; };
const int RAIN_COUNT = 140;
RainDrop rain[RAIN_COUNT];

struct StormCloud { float x, y, scale, speed; };
const int STORM_CLOUD_COUNT = 6;
StormCloud stormClouds[STORM_CLOUD_COUNT];

// ---- Airplane (MANUAL - key 8 = plane flies in, key 9 = plane off) -----------
bool  planeActive = false;
float planeX      = -260.0f;
const float PLANE_Y     = 528.0f;  // cruising height, above the buildings
const float PLANE_SPEED = 95.0f;

// ---------------------------------------------------------------------------
// Small drawing helpers
// ---------------------------------------------------------------------------
void setColor(const Color& c, float a = 1.0f) { glColor4f(c.r, c.g, c.b, a); }

void drawRect(float x0, float y0, float x1, float y1) {
    glBegin(GL_QUADS);
        glVertex2f(x0, y0);
        glVertex2f(x1, y0);
        glVertex2f(x1, y1);
        glVertex2f(x0, y1);
    glEnd();
}

void drawCircle(float cx, float cy, float r, int segs = 40) {
    glBegin(GL_TRIANGLE_FAN);
        glVertex2f(cx, cy);
        for (int i = 0; i <= segs; ++i) {
            float a = 2.0f * PI * i / segs;
            glVertex2f(cx + cosf(a) * r, cy + sinf(a) * r);
        }
    glEnd();
}

void drawCircleOutline(float cx, float cy, float r, int segs = 40) {
    glBegin(GL_LINE_LOOP);
        for (int i = 0; i < segs; ++i) {
            float a = 2.0f * PI * i / segs;
            glVertex2f(cx + cosf(a) * r, cy + sinf(a) * r);
        }
    glEnd();
}

void drawTriangle(float x0,float y0,float x1,float y1,float x2,float y2){
    glBegin(GL_TRIANGLES);
        glVertex2f(x0,y0); glVertex2f(x1,y1); glVertex2f(x2,y2);
    glEnd();
}

void drawText(float x, float y, const char* text, void* font = GLUT_BITMAP_HELVETICA_12) {
    glRasterPos2f(x, y);
    for (const char* c = text; *c; ++c) glutBitmapCharacter(font, *c);
}

// ---------------------------------------------------------------------------
// Init helpers
// ---------------------------------------------------------------------------
void initBuildings() {
    float x = 10;
    Color palette[] = {
        {0.85f,0.45f,0.35f}, {0.35f,0.55f,0.75f}, {0.55f,0.75f,0.55f},
        {0.75f,0.55f,0.75f}, {0.90f,0.70f,0.30f}, {0.45f,0.45f,0.60f},
        {0.70f,0.40f,0.40f}, {0.40f,0.65f,0.65f}, {0.65f,0.50f,0.75f}
    };
    for (int i = 0; i < BUILD_COUNT; ++i) {
        float w = 70 + (rand() % 40);
        float h = 105 + (rand() % 165);   // slightly shorter than before, leaves sky room for the plane
        buildings[i].x = x;
        buildings[i].w = w;
        buildings[i].h = h;
        buildings[i].color = palette[i % 9];
        x += w + 14;
        for (int r = 0; r < 8; ++r)
            for (int c = 0; c < 4; ++c)
                windowLit[i][r][c] = (rand() % 100) < 55;
    }
}

void initStars() {
    for (int i = 0; i < STAR_COUNT; ++i) {
        starX[i] = (float)(rand() % WIN_W);
        starY[i] = 330 + (float)(rand() % 260);
        starTwinkle[i] = (float)(rand() % 100) / 100.0f;
    }
}

void initClouds() {
    for (int i = 0; i < CLOUD_COUNT; ++i) {
        clouds[i].x = (float)(rand() % WIN_W);
        clouds[i].y = 420 + (float)(rand() % 130);
        clouds[i].scale = 0.7f + (rand() % 100) / 100.0f;
        clouds[i].speed = 4.0f + (rand() % 10);
    }
}

void initTrees() {
    float gap = (float)WIN_W / TREE_COUNT;
    for (int i = 0; i < TREE_COUNT; ++i)
        trees[i].x = gap * i + 25 + (rand() % 20);
}

void initRain() {
    for (int i = 0; i < RAIN_COUNT; ++i) {
        rain[i].x = (float)(rand() % WIN_W);
        rain[i].y = (float)(rand() % WIN_H) + WIN_H * 0.3f;
        rain[i].len = 10.0f + (rand() % 12);
        rain[i].speed = 380.0f + (rand() % 220);
    }
    for (int i = 0; i < STORM_CLOUD_COUNT; ++i) {
        stormClouds[i].x = (float)(rand() % (WIN_W + 200)) - 100;
        stormClouds[i].y = 400 + (float)(rand() % 160);
        stormClouds[i].scale = 1.1f + (rand() % 100) / 80.0f;
        stormClouds[i].speed = 8.0f + (rand() % 14);
    }
}

void initCars() {
    Color carColors[] = { {0.85f,0.15f,0.15f}, {0.15f,0.35f,0.80f}, {0.95f,0.80f,0.10f} };
    for (int i = 0; i < CAR_COUNT; ++i) {
        // spread across the whole road at start (instead of bunched far off-screen)
        // so a car is already near the crossing even at slow speeds
        cars[i].x = -50.0f + i * (WIN_W + 100.0f) / CAR_COUNT;
        cars[i].y = ROAD_Y0 + 26;
        cars[i].speed = 60.0f + i * 8.0f;
        cars[i].body = carColors[i];
    }
    Color car2Colors[] = { {0.20f,0.70f,0.30f}, {0.80f,0.45f,0.10f} };
    for (int i = 0; i < CAR2_COUNT; ++i) {
        // spread across the whole road at start (opposite direction)
        cars2[i].x = WIN_W + 50.0f - i * (WIN_W + 100.0f) / CAR2_COUNT;
        cars2[i].y = ROAD_Y0 + 70;
        cars2[i].speed = 55.0f + i * 10.0f;
        cars2[i].body = car2Colors[i];
    }
}

// ---------------------------------------------------------------------------
// Scene drawing
// ---------------------------------------------------------------------------
void drawSky() {
    Color top, bottom;
    if (rainMode) {
        // stormy grey sky, regardless of the underlying day/night phase
        top    = (Color){0.32f, 0.35f, 0.40f};
        bottom = (Color){0.48f, 0.50f, 0.54f};
    } else {
        top    = getSkyColor(dayPhase);
        bottom = lerpColor(top, (Color){1.0f,1.0f,1.0f}, 0.25f);
    }
    glBegin(GL_QUADS);
        setColor(bottom); glVertex2f(0, 0);
        setColor(bottom); glVertex2f(WIN_W, 0);
        setColor(top);    glVertex2f(WIN_W, WIN_H);
        setColor(top);    glVertex2f(0, WIN_H);
    glEnd();
}

// sun / moon travel a big arc across the sky and set/rise at the horizon
void drawSunMoon() {
    if (rainMode) return; // no sun, no moon while it's raining -- just clouds

    float night = nightAmount(dayPhase);

    // SUN: visible roughly during first half of the cycle
    if (dayPhase <= 0.53f) {
        float t = dayPhase / 0.53f;                 // 0..1 across the day
        float ang = PI * t;                          // 0 -> PI (rise -> set)
        float x = 60 + (WIN_W - 120) * t;
        float y = 260 + 260 * sinf(ang);              // slowly sinks back to horizon
        float alpha = 1.0f - powf(t, 8.0f);            // gentle fade right at the very end (sunset)
        if (alpha < 0.15f) alpha = 0.15f;
        // glow
        setColor((Color){1.0f, 0.85f, 0.4f}, 0.25f);
        drawCircle(x, y, 46);
        setColor((Color){1.0f, 0.95f, 0.55f}, alpha);
        drawCircle(x, y, 30);
    }

    // MOON: visible roughly during second half of the cycle
    if (dayPhase >= 0.47f) {
        float t = (dayPhase - 0.47f) / 0.53f;
        if (t > 1.0f) t = 1.0f;
        float ang = PI * t;
        float x = 60 + (WIN_W - 120) * t;
        float y = 260 + 250 * sinf(ang);
        float alpha = night;
        setColor((Color){0.85f,0.85f,0.95f}, 0.85f * alpha);
        drawCircle(x, y, 24);
        // craters / crescent shadow
        setColor(getSkyColor(dayPhase), 0.9f * alpha);
        drawCircle(x + 9, y + 4, 20);
    }
}

void drawStars() {
    if (rainMode) return; // overcast rainy sky -- no stars
    float night = nightAmount(dayPhase);
    if (night < 0.05f) return;
    for (int i = 0; i < STAR_COUNT; ++i) {
        float tw = 0.5f + 0.5f * sinf((float)glutGet(GLUT_ELAPSED_TIME) * 0.002f + starTwinkle[i] * 10.0f);
        setColor((Color){1,1,1}, night * tw);
        drawCircle(starX[i], starY[i], 1.6f, 8);
    }
}

void drawClouds() {
    if (rainMode) return; // fair-weather clouds are swapped for storm clouds
    float night = nightAmount(dayPhase);
    Color c = lerpColor((Color){1,1,1}, (Color){0.55f,0.55f,0.65f}, night);
    for (int i = 0; i < CLOUD_COUNT; ++i) {
        setColor(c, 0.85f);
        float x = clouds[i].x, y = clouds[i].y, s = clouds[i].scale;
        drawCircle(x, y, 18 * s);
        drawCircle(x + 20 * s, y + 6 * s, 22 * s);
        drawCircle(x - 20 * s, y + 4 * s, 16 * s);
        drawCircle(x + 40 * s, y, 14 * s);
    }
}

// thick dark storm clouds that roll across the sky during Rain mode
void drawStormClouds() {
    if (!rainMode) return;
    for (int i = 0; i < STORM_CLOUD_COUNT; ++i) {
        float x = stormClouds[i].x, y = stormClouds[i].y, s = stormClouds[i].scale;
        setColor((Color){0.22f, 0.23f, 0.27f}, 0.55f);
        drawCircle(x, y, 30 * s);
        drawCircle(x + 32 * s, y + 8 * s, 36 * s);
        drawCircle(x - 32 * s, y + 6 * s, 28 * s);
        drawCircle(x + 65 * s, y, 22 * s);
        drawCircle(x - 60 * s, y - 4 * s, 20 * s);
        setColor((Color){0.35f, 0.36f, 0.40f}, 0.6f);
        drawCircle(x, y + 10 * s, 24 * s);
    }
}

// bigger, more detailed airplane that cruises above the buildings while active (key 8/9)
void drawPlane() {
    if (!planeActive) return;
    float x = planeX, y = PLANE_Y;
    float night = nightAmount(dayPhase);

    // long fading contrail behind the plane
    for (int i = 0; i < 5; ++i) {
        float a = 0.20f - i * 0.035f;
        if (a < 0.0f) a = 0.0f;
        setColor((Color){1,1,1}, a);
        drawRect(x - (i + 1) * 46.0f, y + 9, x - i * 46.0f, y + 13);
    }

    Color body      = (Color){0.90f, 0.91f, 0.94f};
    Color wingColor = (Color){0.72f, 0.75f, 0.80f};
    Color accent    = (Color){0.30f, 0.42f, 0.68f};

    // fuselage + nose cone
    setColor(body);
    drawRect(x, y, x + 132, y + 22);
    drawTriangle(x + 132, y, x + 164, y + 11, x + 132, y + 22);

    // vertical tail fin
    setColor(accent);
    drawTriangle(x, y + 22, x + 30, y + 22, x + 12, y + 50);

    // horizontal tail stabilizers
    setColor(wingColor);
    drawTriangle(x + 2, y,      x + 24, y,      x + 12, y - 16);
    drawTriangle(x + 2, y + 22, x + 24, y + 22,  x + 12, y + 38);

    // main wings, swept back
    setColor(wingColor);
    drawTriangle(x + 46, y,      x + 92, y,      x + 66, y - 36);
    drawTriangle(x + 46, y + 22, x + 92, y + 22,  x + 66, y + 58);

    // winglets at the wingtips
    setColor((Color){0.55f, 0.58f, 0.64f});
    drawTriangle(x + 61, y - 36, x + 71, y - 36, x + 66, y - 48);
    drawTriangle(x + 61, y + 58, x + 71, y + 58, x + 66, y + 70);

    // engine nacelles under/over each wing
    setColor((Color){0.30f, 0.32f, 0.36f});
    drawRect(x + 52, y - 24, x + 76, y - 8);
    drawRect(x + 52, y + 30, x + 76, y + 46);
    setColor((Color){0.08f, 0.08f, 0.10f});
    drawCircle(x + 52, y - 16, 7, 16);
    drawCircle(x + 52, y + 38, 7, 16);

    // subtle metallic highlight along the fuselage top
    setColor((Color){1, 1, 1}, 0.35f);
    drawRect(x + 6, y + 17, x + 126, y + 20);

    // livery accent stripe
    setColor(accent);
    drawRect(x + 6, y + 4, x + 132, y + 8);

    // cockpit windshield
    setColor((Color){0.35f, 0.55f, 0.72f}, 0.9f);
    drawTriangle(x + 128, y + 6, x + 146, y + 11, x + 128, y + 16);

    // cabin windows
    setColor((Color){0.25f, 0.35f, 0.45f}, 0.85f);
    for (float wx = x + 20; wx < x + 122; wx += 11)
        drawCircle(wx, y + 12, 2.6f, 10);

    // nav / strobe lights (brighter and blinking, more visible at night)
    float blink = (sinf((float)glutGet(GLUT_ELAPSED_TIME) * 0.012f) > 0) ? 1.0f : 0.2f;
    float glow  = 0.4f + 0.6f * night;
    setColor((Color){1.0f, 0.15f, 0.15f}, glow * blink);              // red - port wingtip
    drawCircle(x + 66, y - 48, 3.0f, 10);
    setColor((Color){0.15f, 1.0f, 0.25f}, glow * blink);              // green - starboard wingtip
    drawCircle(x + 66, y + 70, 3.0f, 10);
    setColor((Color){1.0f, 1.0f, 1.0f}, glow * (1.0f - blink + 0.15f)); // white tail strobe
    drawCircle(x + 12, y + 50, 2.5f, 10);
}

// falling rain drops (drawn as short diagonal streaks)
void drawRain() {
    if (!rainMode) return;https://github.com/shahikintazer/SMART-CITY.git
    setColor((Color){0.75f, 0.85f, 0.95f}, 0.55f);
    glLineWidth(1.5f);
    glBegin(GL_LINES);
        for (int i = 0; i < RAIN_COUNT; ++i) {
            float x = rain[i].x, y = rain[i].y, len = rain[i].len;
            glVertex2f(x, y);
            glVertex2f(x - len * 0.25f, y - len);
        }
    glEnd();
}

void drawBuildings() {
    float night = nightAmount(dayPhase);
    for (int i = 0; i < BUILD_COUNT; ++i) {
        Building& b = buildings[i];
        Color shaded = lerpColor(b.color, (Color){0.08f,0.08f,0.15f}, night * 0.55f);
        setColor(shaded);
        drawRect(b.x, BUILD_BASE, b.x + b.w, BUILD_BASE + b.h);

        // rooftop trim
        setColor(lerpColor(shaded, (Color){0,0,0}, 0.25f));
        drawRect(b.x - 3, BUILD_BASE + b.h, b.x + b.w + 3, BUILD_BASE + b.h + 8);

        // windows
        int cols = (int)(b.w / 18);
        int rows = (int)(b.h / 30);
        if (cols > 4) cols = 4;
        if (rows > 8) rows = 8;
        float wW = 10, wH = 14;
        for (int r = 0; r < rows; ++r) {
            for (int c = 0; c < cols; ++c) {
                float wx = b.x + 10 + c * (b.w - 20) / (cols > 1 ? (cols - 1) : 1);
                float wy = BUILD_BASE + 14 + r * (b.h - 24) / (rows > 1 ? (rows - 1) : 1);
                bool lit = windowLit[i][r][c];
                if (lit && night > 0.15f)
                    setColor((Color){1.0f, 0.85f, 0.35f}, 0.55f + 0.45f * night);
                else
                    setColor(lerpColor((Color){0.6f,0.75f,0.85f}, (Color){0.05f,0.05f,0.1f}, night*0.7f), 0.7f);
                drawRect(wx, wy, wx + wW, wy + wH);
            }
        }
    }
}

void drawTrees() {
    for (int i = 0; i < TREE_COUNT; ++i) {
        float x = trees[i].x;
        float night = nightAmount(dayPhase);
        setColor(lerpColor((Color){0.45f,0.28f,0.12f}, (Color){0.1f,0.06f,0.03f}, night*0.6f));
        drawRect(x - 4, GRASS_Y0, x + 4, GRASS_Y0 + 22);
        setColor(lerpColor((Color){0.25f,0.65f,0.25f}, (Color){0.05f,0.2f,0.08f}, night*0.6f));
        drawCircle(x, GRASS_Y0 + 34, 18);
        drawCircle(x - 12, GRASS_Y0 + 26, 13);
        drawCircle(x + 12, GRASS_Y0 + 26, 13);
    }
}

void drawGroundBands() {
    float night = nightAmount(dayPhase);
    // grass
    setColor(lerpColor((Color){0.35f,0.65f,0.30f}, (Color){0.05f,0.12f,0.06f}, night*0.6f));
    drawRect(0, GRASS_Y0, WIN_W, GRASS_Y1);
    // rail bed
    setColor(lerpColor((Color){0.55f,0.50f,0.42f}, (Color){0.12f,0.11f,0.10f}, night*0.6f));
    drawRect(0, RAIL_Y0, WIN_W, RAIL_Y1);
    // rail tracks (two steel rails + sleepers)
    setColor((Color){0.15f,0.15f,0.15f});
    for (float x = 0; x < WIN_W; x += 26)
        drawRect(x, RAIL_Y0 + 4, x + 14, RAIL_Y1 - 4);
    setColor((Color){0.75f,0.75f,0.78f});
    drawRect(0, RAIL_Y0 + 8, WIN_W, RAIL_Y0 + 12);
    drawRect(0, RAIL_Y1 - 12, WIN_W, RAIL_Y1 - 8);
    // sidewalk
    setColor(lerpColor((Color){0.75f,0.75f,0.72f}, (Color){0.18f,0.18f,0.20f}, night*0.6f));
    drawRect(0, SIDE_Y0, WIN_W, SIDE_Y1);
    // road
    setColor(lerpColor((Color){0.20f,0.20f,0.22f}, (Color){0.05f,0.05f,0.07f}, night*0.5f));
    drawRect(0, ROAD_Y0, WIN_W, ROAD_Y1);
    // lane divider (dashed)
    setColor((Color){0.95f,0.85f,0.25f});
    float phase = fmodf((float)glutGet(GLUT_ELAPSED_TIME) * 0.03f, 40.0f);
    for (float x = -40 + phase; x < WIN_W; x += 40)
        drawRect(x, ROAD_Y0 + 53, x + 20, ROAD_Y0 + 57);
}

void drawStreetLamps() {
    float night = nightAmount(dayPhase);
    for (float x = 60; x < WIN_W; x += 220) {
        setColor((Color){0.2f,0.2f,0.22f});
        drawRect(x - 3, SIDE_Y0, x + 3, SIDE_Y0 + 46);
        Color glow = night > 0.2f ? (Color){1.0f, 0.92f, 0.55f} : (Color){0.5f,0.5f,0.5f};
        setColor(glow, night > 0.2f ? 0.35f : 0.0f);
        drawCircle(x, SIDE_Y0 + 48, 14);
        setColor(glow, night > 0.2f ? 1.0f : 0.5f);
        drawCircle(x, SIDE_Y0 + 48, 6);
    }
}

void drawTrafficLight() {
    // pole
    setColor((Color){0.15f,0.15f,0.15f});
    drawRect(SIGNAL_POLE_X - 4, SIDE_Y0, SIGNAL_POLE_X + 4, SIDE_Y0 + 110);
    // housing
    setColor((Color){0.10f,0.10f,0.10f});
    drawRect(SIGNAL_POLE_X - 16, SIDE_Y0 + 100, SIGNAL_POLE_X + 16, SIDE_Y0 + 170);
    // lights
    Color off = {0.25f, 0.08f, 0.08f};
    Color red    = (lightState == LS_RED)    ? (Color){1.0f,0.15f,0.1f} : off;
    Color yellow = (lightState == LS_YELLOW) ? (Color){1.0f,0.9f,0.1f} : (Color){0.3f,0.28f,0.05f};
    Color green  = (lightState == LS_GREEN)  ? (Color){0.15f,1.0f,0.2f} : (Color){0.05f,0.25f,0.08f};
    setColor(red);    drawCircle(SIGNAL_POLE_X, SIDE_Y0 + 155, 8);
    setColor(yellow); drawCircle(SIGNAL_POLE_X, SIDE_Y0 + 135, 8);
    setColor(green);  drawCircle(SIGNAL_POLE_X, SIDE_Y0 + 115, 8);
}

void drawWarningLight() {
    bool warning = trainRunning && (railTimer >= WARN_START && railTimer <= GATE_OPEN_END);
    float blink = (sinf((float)glutGet(GLUT_ELAPSED_TIME) * 0.02f) > 0) ? 1.0f : 0.25f;
    setColor((Color){0.15f,0.15f,0.15f});
    drawRect(GATE_X - 4, SIDE_Y0, GATE_X + 4, SIDE_Y0 + 40);
    setColor((Color){1.0f,0.15f,0.1f}, warning ? blink : 0.15f);
    drawCircle(GATE_X - 10, SIDE_Y0 + 42, 6);
    setColor((Color){1.0f,0.15f,0.1f}, warning ? (1.0f - blink + 0.15f) : 0.15f);
    drawCircle(GATE_X + 10, SIDE_Y0 + 42, 6);
}

// gate arm rotates from vertical (open) to horizontal (closed) about a pivot
void drawGate() {
    glPushMatrix();
        glTranslatef(GATE_X, SIDE_Y1, 0);
        glRotatef(-gateAngle, 0, 0, 1);
        setColor((Color){0.9f, 0.15f, 0.15f});
        drawRect(0, -4, 90, 4);
        setColor((Color){1.0f, 1.0f, 1.0f});
        for (float s = 6; s < 84; s += 18) drawRect(s, -4, s + 8, 4);
    glPopMatrix();
    // pivot post
    setColor((Color){0.15f,0.15f,0.15f});
    drawRect(GATE_X - 4, SIDE_Y0, GATE_X + 4, SIDE_Y1 + 6);
}

void drawTrain() {
    if (!trainRunning) return;
    if (railTimer < TRAIN_START || railTimer > TRAIN_END) return;
    float t = (railTimer - TRAIN_START) / (TRAIN_END - TRAIN_START);
    float x = -140 + (WIN_W + 280) * t;
    float y = RAIL_Y0 + 4;
    // engine + carriages
    for (int i = 0; i < 4; ++i) {
        float cx = x - i * 100;
        Color body = (i == 0) ? (Color){0.75f,0.15f,0.15f} : (Color){0.25f,0.35f,0.65f};
        setColor(body);
        drawRect(cx, y, cx + 86, y + 30);
        setColor((Color){0.6f,0.85f,0.95f}, 0.85f);
        for (float wx = cx + 10; wx < cx + 76; wx += 22)
            drawRect(wx, y + 16, wx + 12, y + 26);
        setColor((Color){0.1f,0.1f,0.1f});
        drawCircle(cx + 16, y - 2, 6, 16);
        drawCircle(cx + 68, y - 2, 6, 16);
    }
}

void drawCarShape(float x, float y, const Color& body, bool facingRight = true) {
    setColor(body);
    drawRect(x, y, x + 60, y + 20);
    drawRect(x + 12, y + 20, x + 46, y + 34);
    setColor((Color){0.6f,0.85f,0.95f}, 0.9f);
    drawRect(x + 16, y + 21, x + 42, y + 32);
    setColor((Color){0.05f,0.05f,0.05f});
    drawCircle(x + 14, y - 2, 8, 16);
    drawCircle(x + 46, y - 2, 8, 16);
    // headlight glow at night
    float night = nightAmount(dayPhase);
    if (night > 0.3f) {
        setColor((Color){1.0f,0.95f,0.6f}, 0.6f * night);
        if (facingRight) drawCircle(x + 60, y + 8, 5);
        else drawCircle(x, y + 8, 5);
    }
}

void drawCars() {
    for (int i = 0; i < CAR_COUNT; ++i)
        drawCarShape(cars[i].x, cars[i].y, cars[i].body, true);
    for (int i = 0; i < CAR2_COUNT; ++i)
        drawCarShape(cars2[i].x - 60, cars2[i].y, cars2[i].body, false);
}

void drawHUD() {
    setColor((Color){0,0,0}, 0.4f);
    drawRect(10, WIN_H - 136, 370, WIN_H - 10);

    setColor((Color){1,1,1});
    char buf[128];
    drawText(20, WIN_H - 28, "TIME ESCAPE - Smart City Simulation", GLUT_BITMAP_HELVETICA_18);

    const char* timeLabel = rainMode ? "RAINY" : (sceneState == STATE_NIGHT ? "NIGHT" : "DAY");
    snprintf(buf, sizeof(buf), "Sky: %s%s", timeLabel, transitioning ? "  (changing...)" : "");
    drawText(20, WIN_H - 48, buf);

    const char* lState = (lightState == LS_GREEN) ? "GREEN - GO" : (lightState == LS_YELLOW) ? "YELLOW - SLOW" : "RED - STOP";
    snprintf(buf, sizeof(buf), "Traffic signal: %s", lState);
    drawText(20, WIN_H - 66, buf);

    const char* gState = (gateAngle > 5.0f) ? "CLOSED (train crossing)" : "OPEN";
    snprintf(buf, sizeof(buf), "Railway gate: %s", gState);
    drawText(20, WIN_H - 84, buf);

    snprintf(buf, sizeof(buf), "Train: %s", trainRunning ? "RUNNING" : "STOPPED");
    drawText(20, WIN_H - 102, buf);

    snprintf(buf, sizeof(buf), "Plane: %s", planeActive ? "FLYING" : "STOPPED");
    drawText(20, WIN_H - 120, buf);

    if (paused) drawText(280, WIN_H - 120, "[PAUSED]");

    drawText(WIN_W - 330, WIN_H - 20, "1=Night 2=Day 3=Rain 4=TrainOn 5=TrainOff");
    drawText(WIN_W - 330, WIN_H - 36, "6=RedLight 7=GreenLight 8=PlaneOn 9=PlaneOff");
    drawText(WIN_W - 330, WIN_H - 52, "P=pause R=reset ESC=quit");
}

// ---------------------------------------------------------------------------
// Update / physics
// ---------------------------------------------------------------------------
// Kick off a smooth animated move of the sun/moon toward targetPhase.
// If targetPhase would be "behind" the current phase, we push it forward
// by a full lap (+1.0) so the sky always animates FORWARD in time --
// e.g. night(0.78) -> day(0.20) actually plays as 0.78 -> 1.20 (wraps to
// 0.20), so the moon visibly sets and the sun visibly rises, instead of
// the animation looking like it's rewinding.
void startTransition(float targetPhase) {
    transStart = dayPhase;
    transEnd   = targetPhase;
    if (transEnd <= transStart + 0.001f) transEnd += 1.0f;
    transElapsed  = 0.0f;
    transitioning = true;
}

void updateDayNight(float dt) {
    if (!transitioning) return;
    transElapsed += dt;
    float t = transElapsed / transDuration;
    if (t >= 1.0f) { t = 1.0f; transitioning = false; }
    float eased = t * t * (3.0f - 2.0f * t); // smoothstep, nicer than linear
    float unwrapped = transStart + (transEnd - transStart) * eased;
    dayPhase = fmodf(unwrapped, 1.0f);
    if (dayPhase < 0.0f) dayPhase += 1.0f;
}

void updateRain(float dt) {
    if (!rainMode) return;
    for (int i = 0; i < RAIN_COUNT; ++i) {
        rain[i].y -= rain[i].speed * dt;
        rain[i].x -= rain[i].speed * dt * 0.1f;
        if (rain[i].y < 0) {
            rain[i].y = WIN_H + (rand() % 60);
            rain[i].x = (float)(rand() % (WIN_W + 100)) - 50;
        }
    }
    for (int i = 0; i < STORM_CLOUD_COUNT; ++i) {
        stormClouds[i].x += stormClouds[i].speed * dt;
        if (stormClouds[i].x > WIN_W + 120) stormClouds[i].x = -120;
    }
}

// Traffic signal is now fully MANUAL (key 6 = red, key 7 = green), so there
// is no automatic timer-based cycling any more -- lightState only changes
// when the keyboard() handler sets it directly.

void updateRailway(float dt) {
    if (!trainRunning) {
        // train is switched off: keep the gate open and the timer at rest
        gateAngle = 0.0f;
        railTimer = 0.0f;
        return;
    }

    railTimer += dt;
    if (railTimer >= CYCLE_LEN) railTimer -= CYCLE_LEN; // loop the crossing sequence while running

    if (railTimer >= WARN_START && railTimer < GATE_CLOSE_END) {
        float t = (railTimer - WARN_START) / (GATE_CLOSE_END - WARN_START);
        gateAngle = 90.0f * t;
    } else if (railTimer >= GATE_CLOSE_END && railTimer < GATE_OPEN_START) {
        gateAngle = 90.0f;
    } else if (railTimer >= GATE_OPEN_START && railTimer < GATE_OPEN_END) {
        float t = (railTimer - GATE_OPEN_START) / (GATE_OPEN_END - GATE_OPEN_START);
        gateAngle = 90.0f * (1.0f - t);
    } else {
        gateAngle = 0.0f;
    }
}

void updateCars(float dt) {
    bool gateClosed = gateAngle > 5.0f;
    for (int i = 0; i < CAR_COUNT; ++i) {
        Car& c = cars[i];
        bool mustStopAtSignal = (c.x + 60 >= SIGNAL_STOP_X - 4) && (c.x <= SIGNAL_STOP_X) && (lightState != LS_GREEN);
        bool mustStopAtGate   = (c.x + 60 >= GATE_STOP_X - 4)   && (c.x <= GATE_STOP_X)   && gateClosed;
        if (!mustStopAtSignal && !mustStopAtGate)
            c.x += c.speed * dt;
        if (c.x > WIN_W + 80) c.x = -40.0f - (rand() % 60); // shorter respawn distance
    }
    for (int i = 0; i < CAR2_COUNT; ++i) {
        Car2& c = cars2[i];
        // cars2 move right-to-left; its drawn body spans [x-60, x], so the
        // "front" bumper while moving left is at (x - 60).
        bool mustStopAtGate = (c.x - 60 <= GATE_STOP_X2 + 4) && (c.x >= GATE_STOP_X2) && gateClosed;
        if (!mustStopAtGate)
            c.x -= c.speed * dt;
        if (c.x < -180) c.x = WIN_W + 40.0f + (rand() % 60); // shorter respawn distance
    }
}

void updateClouds(float dt) {
    for (int i = 0; i < CLOUD_COUNT; ++i) {
        clouds[i].x += clouds[i].speed * dt;
        if (clouds[i].x > WIN_W + 80) clouds[i].x = -80;
    }
}

void updatePlane(float dt) {
    if (!planeActive) return;
    planeX += PLANE_SPEED * dt;
    if (planeX > WIN_W + 60.0f) planeX = -260.0f; // loops across the sky while active
}

// ---------------------------------------------------------------------------
// GLUT callbacks
// ---------------------------------------------------------------------------
int lastTimeMs = 0;

void display() {
    glClear(GL_COLOR_BUFFER_BIT);
    glLoadIdentity();

    drawSky();
    drawSunMoon();
    drawStars();
    drawClouds();
    drawStormClouds();
    drawPlane();
    drawBuildings();
    drawGroundBands();
    drawTrees();
    drawStreetLamps();
    drawWarningLight();
    drawTrafficLight();
    drawTrain();
    drawGate();
    drawCars();
    drawRain();
    drawHUD();

    glutSwapBuffers();
}

void timerFunc(int) {
    int now = glutGet(GLUT_ELAPSED_TIME);
    float dt = (lastTimeMs == 0) ? 0.016f : (now - lastTimeMs) / 1000.0f;
    if (dt > 0.05f) dt = 0.05f; // clamp on hiccups
    lastTimeMs = now;

    if (!paused) {
        updateDayNight(dt);
        // traffic light is manual now (keys 6/7) -- nothing to auto-update
        updateRailway(dt);
        updateCars(dt);
        updateClouds(dt);
        updatePlane(dt);
        updateRain(dt);
    }

    glutPostRedisplay();
    glutTimerFunc(16, timerFunc, 0);
}

void reshape(int w, int h) {
    if (h == 0) h = 1;
    glViewport(0, 0, w, h);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    gluOrtho2D(0, WIN_W, 0, WIN_H);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
}

void resetSimulation() {
    dayPhase = PHASE_DAY;
    sceneState = STATE_DAY;
    transitioning = false;
    transDuration = 4.0f;
    rainMode = false;
    lightState = LS_GREEN;
    trainRunning = false;
    railTimer = 0; gateAngle = 0;
    planeActive = false;
    planeX = -260.0f;
    initCars();
}

void keyboard(unsigned char key, int, int) {
    switch (key) {
        case '1': // DAY -> NIGHT : sun sets, moon rises
            rainMode = false;
            if (sceneState != STATE_NIGHT || transitioning) {
                sceneState = STATE_NIGHT;
                startTransition(PHASE_NIGHT);
                printf(">> Transition started: DAY -> NIGHT (sun setting, moon rising)\n");
            }
            break;
        case '2': // NIGHT -> DAY : moon sets, sun rises
            rainMode = false;
            if (sceneState != STATE_DAY || transitioning) {
                sceneState = STATE_DAY;
                startTransition(PHASE_DAY);
                printf(">> Transition started: NIGHT -> DAY (moon setting, sun rising)\n");
            }
            break;
        case '3': // RAIN : no sun, no moon, just dark clouds and rain
            if (!rainMode) {
                rainMode = true;
                transitioning = false;
                initRain();
                printf(">> Rain mode activated (sun & moon hidden, dark clouds + rain)\n");
            }
            break;
        case '4': // TRAIN ON : the railway crossing sequence starts running
            if (!trainRunning) {
                trainRunning = true;
                railTimer = 0.0f;
                printf(">> Train STARTED (warning -> gate closes -> train crosses -> gate opens)\n");
            }
            break;
        case '5': // TRAIN OFF : train stops immediately, gate opens right away
            if (trainRunning) {
                trainRunning = false;
                railTimer = 0.0f;
                gateAngle = 0.0f;
                printf(">> Train STOPPED (gate open, track clear)\n");
            }
            break;
        case '6': // SIGNAL RED : cars stop at the signal line
            lightState = LS_RED;
            printf(">> Traffic signal -> RED (cars stopping)\n");
            break;
        case '7': // SIGNAL GREEN : cars resume driving
            lightState = LS_GREEN;
            printf(">> Traffic signal -> GREEN (cars moving)\n");
            break;
        case '8': // PLANE ON : plane flies in from the left and loops across the sky
            if (!planeActive) {
                planeActive = true;
                planeX = -260.0f;
                printf(">> Plane STARTED (cruising across the sky)\n");
            }
            break;
        case '9': // PLANE OFF : plane disappears
            if (planeActive) {
                planeActive = false;
                printf(">> Plane STOPPED (removed from the sky)\n");
            }
            break;
        case 'p': case 'P':
            paused = !paused;
            printf(">> %s\n", paused ? "Simulation PAUSED" : "Simulation RESUMED");
            break;
        case '+': case '=':
            transDuration = (transDuration / 1.4f > 1.0f) ? transDuration / 1.4f : 1.0f;
            printf(">> Transition speed increased (duration now %.1fs)\n", transDuration);
            break;
        case '-': case '_':
            transDuration = (transDuration * 1.4f < 15.0f) ? transDuration * 1.4f : 15.0f;
            printf(">> Transition speed decreased (duration now %.1fs)\n", transDuration);
            break;
        case 'r': case 'R':
            resetSimulation();
            printf(">> Simulation reset\n");
            break;
        case 27: exit(0); break; // ESC
    }
}

void initGL() {
    glClearColor(0, 0, 0, 1);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    srand((unsigned)time(0));
    initBuildings();
    initStars();
    initClouds();
    initTrees();
    initRain();
    initCars();
}

void printControls() {
    printf("===============================================================\n");
    printf("  TIME ESCAPE - Smart City Simulation - KEYBOARD CONTROLS\n");
    printf("===============================================================\n");
    printf("   1  ->  DAY to NIGHT    (sun sets, moon rises)\n");
    printf("   2  ->  NIGHT to DAY    (moon sets, sun rises)\n");
    printf("   3  ->  RAIN MODE       (dark clouds + rain, no sun/moon)\n");
    printf("   4  ->  TRAIN ON        (railway crossing sequence starts)\n");
    printf("   5  ->  TRAIN OFF       (train stops, gate opens immediately)\n");
    printf("   6  ->  SIGNAL RED      (cars stop at the traffic light)\n");
    printf("   7  ->  SIGNAL GREEN    (cars resume driving)\n");
    printf("   8  ->  PLANE ON        (plane flies in and loops across the sky)\n");
    printf("   9  ->  PLANE OFF       (plane disappears)\n");
    printf("   P  ->  Pause / Resume the whole simulation\n");
    printf("   +  ->  Make the sunrise/sunset transition faster\n");
    printf("   -  ->  Make the sunrise/sunset transition slower\n");
    printf("   R  ->  Reset the simulation\n");
    printf("  ESC ->  Quit\n");
    printf("===============================================================\n");
    printf("  The scene starts as DAY, signal GREEN, train STOPPED, and\n");
    printf("  nothing changes on its own -- everything is controlled by\n");
    printf("  the keys above.\n");
    printf("===============================================================\n\n");
}

int main(int argc, char** argv) {
    setvbuf(stdout, NULL, _IONBF, 0); // make sure printf shows up immediately in the console
    glutInit(&argc, argv);
    glutInitDisplayMode(GLUT_DOUBLE | GLUT_RGBA);
    glutInitWindowSize(WIN_W, WIN_H);
    glutInitWindowPosition(80, 60);
    glutCreateWindow("Time Escape - Smart City Transportation & Environment Simulation");

    initGL();
    printControls();

    glutDisplayFunc(display);
    glutReshapeFunc(reshape);
    glutKeyboardFunc(keyboard);
    glutTimerFunc(16, timerFunc, 0);

    glutMainLoop();
    return 0;
}