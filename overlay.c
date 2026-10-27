/* SPDX-License-Identifier: GPL-2.0-or-later */
/* SampleTone's pages: full SYN1/SYN2 content redraws on a machine track.
 *
 * The stock SYN pages draw FM's pictures (algorithm diagram, envelope graphs)
 * and format values as FM, which core-dn1 3.1 cannot replace per track, so this
 * paints the content rect on ev_draw with our labels, true values and a
 * waveform with start/end markers. Title bar, left rail and status stay stock.
 *
 * Gating is firmware state only, never pixels: the active track's sound plays
 * SampleTone (sound slot 0), the showing key is SYN1 or SYN2, no screen is over
 * the pages, and the shown view is on its first page. Second pages, menus,
 * browsers and the Mod Menu fail those checks.
 *
 * A held sequencer trig in GRID or STEP record shows that step's parameter
 * locks instead of the kit, inverted where locked, as the manual's p-lock pages
 * do; a hold in LIVE record or with the transport stopped plays notes and shows
 * the kit, as stock. The lock row and the record-mode tracking follow the
 * pattern's own lock table (see below), measured on Digitone hardware.
 *
 * The showing key's address and the brain/screen chain are measured Digitone
 * 1.44 values (KEYVAR_AT 0x41368ce4 in 1.44). They are
 * cflags so a future OS port replaces them in mod.json, not in code.
 *
 * This file runs on the UI task; it never takes a key or encoder event. */
#include "digitone-mk1/machines.h"
#include "samples.h"

#define ST_ID 81

#ifndef KEYVAR_AT
#error "KEYVAR_AT: the showing key's address (0x41368ce4 in 1.44)"
#endif

/* Brain/screen chain, as core_page_shown uses it (core-dn1 menu.s): the key
 * dispatcher's brain keeps the screens shown at +0x54 (bottom) and +0x5c (top);
 * the bottom screen's page manager is at +8 and its shown view at +0xc0; a
 * view's place in its list is at +144. */
#define BRAIN_BOTTOM 0x54
#define BRAIN_TOP    0x5c
#define SCREEN_MGR   8
#define MGR_VIEW     0xc0
#define VIEW_CUR     144

/* Content area and grid (panel coords, y = 0 at the top), the rail-safe
 * rectangle measured against the stock page: clearing from x15 takes the
 * animated algorithm diagram's bleed at ALGO 7/8 without touching the rail at
 * x14. */
#define CX0  18
#define CXC  15
#define CX1  127
#define CY0  12
#define CY1  63
#define COLS 4
#define ROWS 2

/* The eight knob slots of a SYN page, in knob order: SYN1 A-H are stock slots
 * 17-24, SYN2 A-H slots 27-34 (measured on 1.44). */
static const uint8_t st_slots[16] = {
    17, 18, 19, 20, 21, 22, 23, 24,
    27, 28, 29, 30, 31, 32, 33, 34,
};

extern const struct dnm_knob st_knobs[DNM_KNOBS];
extern const int16_t st_defaults[DNM_KNOBS];

/* ---- FUNC+turn defaults ---------------------------------------------------------------------- */
/* FUNC and a knob's turn resets it to its parameter record's default (the
 * firmware's own defaults, e.g. FDBK's 0). The records are one table for all
 * four tracks, so the constant tick holds these three knobs at the machine's
 * default while the active track plays SampleTone and puts the stock value
 * back on every other track: on an FM track FUNC+turn is the stock one again.
 * The three the machine defaults away from FM's are END, LEVEL and CRSH; the
 * stock values are read on the first tick, before anything here writes them. */
#define PREC        60
#define REC_DEFAULT 16
#define DEF_STOCK   (-1)

/* The parameter records (core3.h's fw_params). A host test points this at its
 * own buffer: the firmware's table is declared const. */
#ifndef ST_PARAM_TABLE
#define ST_PARAM_TABLE ((const uint8_t *)fw_params)
#endif

static int32_t stock_default[3] = { DEF_STOCK, DEF_STOCK, DEF_STOCK };
static const uint8_t default_ids[3] = { 113, 114, 126 };    /* FDBK, MIX, B Level */
static const uint8_t default_knobs[3] = { 6, 7, 15 };       /* END, LEVEL, CRSH */

static void keep_defaults(int32_t mid)
{
    int32_t machine = mid == ST_ID, i;
    for (i = 0; i < 3; i++) {
        volatile int32_t *def = (volatile int32_t *)(ST_PARAM_TABLE +
            PREC * default_ids[i] + REC_DEFAULT);
        int32_t want;
        if (stock_default[i] == DEF_STOCK)
            stock_default[i] = *def;    /* the first look: still the stock one */
        want = machine ? st_defaults[default_knobs[i]] : stock_default[i];
        if (*def != want)
            *def = want;
    }
}

static uint32_t st_frames;
static uint32_t st_ticks;
static uint32_t st_enc_at;
static int32_t st_enc = -1;
static int32_t st_enc_page = -1;
static void *st_brain;

/* Popups and toasts are drawn in this same view (no new brain screen), so the
 * brain check cannot catch them. Only the chords that actually draw one yield
 * the frame, for ~90 ticks:
 *   - PTN and BANK, in any mode ("BANK A : SELECT PTN", "SELECT BANK A..H");
 *   - FUNC+SETTINGS, the sound browser, whose first frames are drawn over the
 *     page before its own screen lands;
 *   - in a record mode (GRID, STEP or LIVE), a copy/paste/clear chord: FUNC, a
 *     step trig or a track key held with RECORD, PLAY or STOP.
 *
 * Everything else keeps the page up: plain RECORD, PLAY and STOP, RECORD+PLAY
 * or RECORD+STOP, which only change the record mode and draw no popup, and
 * plain SETTINGS, which opens its own screen (the brain check sees that).
 *
 * Key events carry the firmware's own flags: 0x01 press, 0x02 a modifier is
 * held (FUNC: it is the chord bit of the RECORD/PLAY/STOP press itself), 0x08
 * auto-repeat, 0x10 release. A release can also arrive with no flags at all:
 * when a mode change takes a held key away (RECORD crossing into a record mode,
 * a track press switching track or opening the Mod Menu) the firmware flushes
 * that key with flags 0. So a press is "bit 0 set, bit 4 clear" and everything
 * else is a release. Reading flags 0 as a press left FUNC, a trig or a track
 * key tracked as held forever, and then every later RECORD, PLAY or STOP in a
 * record mode hid the page. FUNC is not tracked at all: the chord bit on the
 * press says whether it was down. */
static uint32_t st_trigs;      /* step trig keys held (ids 26-41) */
static uint32_t st_tracks;     /* track keys held (ids 42-45) */
static uint32_t st_menu;       /* PTN (1), BANK (2): their popups eat keys */
static uint32_t st_rec_down;
static uint32_t st_edit_rec;   /* GRID or STEP record: a held trig addresses a step */
static uint32_t st_live;       /* LIVE RECORDING: a held trig plays a note */
static uint32_t st_msg_until;
#define ST_MSG_HOLD 90u

static int32_t msg_suppressed(void)
{
    return (int32_t)(st_ticks - st_msg_until) < 0;
}

/* Does this key edge draw a popup in the page's own view? Pure, so the host
 * test can walk the whole matrix without firmware state. PTN and BANK yield on
 * press and release alike, so a popup that outlives the press stays covered;
 * the record-mode chords yield on the press that runs the command. `chord` is
 * the firmware's own bit for "a modifier is held", not a tracked one. */
int32_t st_popup_chord(int32_t id, int32_t down, int32_t chord, int32_t trigs,
                        int32_t tracks, int32_t edit_rec)
{
    if (id == 4 || id == 5)         /* PTN, BANK */
        return 1;
    if (!down)
        return 0;
    if (id == 7)                    /* SETTINGS: the sound browser on FUNC */
        return chord;
    if (id != 10 && id != 11 && id != 12)
        return 0;
    return edit_rec && (chord || trigs || tracks);
}

/* y_top -> Bitmap y (0 at the bottom). */
#define YB(yt) (63 - (yt))

/* ---- parameter locks ------------------------------------------------------------------------ */
/* The pattern's lock table: track t at +0x1e80 + 10319 t, step at +160 a step,
 * one int16 a sound slot, -1 = no lock. Measured on 1.43, where 1.44 moved
 * only the RAM addresses by 0x1000, so the table itself is unchanged; here the
 * pointers come from core's fw_pattern export, not a hard-coded address. Reads
 * are bounds-checked: a bad pointer or index shows the kit, never a wild
 * value. */
#define LOCK_TAB_OFF    0x1e80u
#define LOCK_TRACK_STR  10319u
#define LOCK_STEP_STR   160u

int32_t st_lock_read(const uint8_t *pattern, int32_t track, int32_t step,
                      int32_t slot)
{
    const uint8_t *row;
    if (!pattern || track < 0 || track > 3 || step < 0 || step > 63 ||
        slot < 0 || slot > 78)
        return -1;
    row = pattern + LOCK_TAB_OFF + (uint32_t)track * LOCK_TRACK_STR +
        (uint32_t)step * LOCK_STEP_STR;
    return (int32_t)*(volatile const int16_t *)(row + (uint32_t)slot * 2u);
}

/* Held step for lock display: the lowest held trig key (ids 26-41), or -1. */
static int32_t held_step(void)
{
    uint32_t m = st_trigs, i;
    if (!m)
        return -1;
    for (i = 0; i < 16; i++)
        if (m & (1u << i))
            return (int32_t)i;
    return -1;
}

/* The step whose locks the page shows: only while editing steps, because a
 * hold otherwise plays a note and the kit is what the knobs show. */
static int32_t shown_step(void)
{
    return st_edit_rec ? held_step() : -1;
}

/* The page's sixteen values: the held step's locks where locked, else the kit. */
static void shown_state(uint8_t *kit, int32_t track, int32_t *vals,
                        uint32_t *lockedmask)
{
    uint8_t *pattern = 0;
    int32_t step = shown_step(), i;
    uint32_t locked = 0;

    if (step >= 0)
        pattern = fw_pattern;
    for (i = 0; i < DNM_KNOBS; i++) {
        int32_t slot = st_slots[i], v = 0x7fffffff;
        if (slot >= 0) {
            v = FW_SOUND_SLOT(kit, track, slot);
            if (pattern) {
                int32_t lv = st_lock_read(pattern, track, step, slot);
                if (lv != -1) {
                    v = lv;
                    locked |= 1u << i;
                }
            }
        }
        vals[i] = v;
    }
    *lockedmask = locked;
}

static void cell_bar(void *bmp, int32_t x, int32_t ytop, int32_t w,
                     int32_t val, int32_t max, int32_t center)
{
    int32_t yb = YB(ytop), fill, f0, f1;
    if (max <= 0)
        return;
    if (val < 0)
        val = 0;
    if (val > max)
        val = max;
    fw_framerect(bmp, x, yb - 4, x + w, yb, 1);
    if (!center) {
        fill = (w * val) / max;
        if (fill > 0)
            fw_fillrect(bmp, x + 1, yb - 3, x + fill, yb - 1, 1);
        return;
    }
    fill = (w * val) / max;
    f0 = x + 1 + w / 2;
    f1 = x + 1 + w / 2 + fill - w / 2;
    if (f1 < f0) {
        int32_t t = f0;
        f0 = f1;
        f1 = t;
    }
    if (f1 > f0)
        fw_fillrect(bmp, f0, yb - 3, f1, yb - 1, 1);
    fw_fillrect(bmp, x + w / 2, yb - 3, x + 1 + w / 2, yb - 1, 1);
}

static void draw_cell(void *bmp, int32_t k, int32_t x, int32_t ytop,
                      int32_t w, int32_t value, int32_t sel, int32_t locked)
{
    char buf[16];
    const char *name = st_knobs[k].name;
    int32_t v = value >> 8;
    if (!name || !name[0])
        return;
    fw_textf(bmp, fw_font5, x + 1, YB(ytop) - 7, -1, "%s", name);
    if (st_knobs[k].fmt)
        st_knobs[k].fmt(value, buf);
    else
        buf[0] = 0;
    fw_textf(bmp, fw_font5, x + 1, YB(ytop) - 14, -1, "%s", buf);
    /* Locked at the held step: invert label and value, as the p-lock pages do.
     * The text block is panel ytop+3..ytop+14 (5-pixel glyphs at +7 and +14),
     * so the band runs ytop+1..ytop+16: two rows of margin above and below, and
     * three rows clear of the mini-bar, which starts at +19. */
    if (locked)
        fw_fillrect(bmp, x, YB(ytop + 16), x + w, YB(ytop + 1), -1);
    if (k == 0)
        cell_bar(bmp, x + 1, ytop + 19, w - 2, v, 7, 0);
    else if (k == 4)
        /* PITCH stores 37..89 (displayed -26..+26). A centred bar spans the
         * whole stored range with its midpoint at 63, so pass 0..52, not
         * -26..+26: the latter put the bar's empty point at +13. */
        cell_bar(bmp, x + 1, ytop + 19, w - 2, v - 37, 52, 1);
    else if (k == 8)
        cell_bar(bmp, x + 1, ytop + 19, w - 2, v, 127, 0);
    else if (k == 15)
        /* CRSH: 127 is OFF at the top, so the bar empties as the knob passes it. */
        cell_bar(bmp, x + 1, ytop + 19, w - 2, v, 127, 0);
    else if (k >= 5 && k <= 7)
        cell_bar(bmp, x + 1, ytop + 19, w - 2, v, 127, 0);
    if (sel)
        fw_framerect(bmp, x, YB(ytop + 17), x + w, YB(ytop), 1);
}

/* The waveform, with start (S) and end (E) markers. Drawn from the same table
 * the engine plays; a future +Drive loader fills that table, so this needs no
 * change then. */
static void draw_wave(void *bmp, int32_t x0, int32_t ytop, int32_t x1,
                      int32_t ybot, uint32_t si, int32_t start, int32_t end)
{
    const struct st_sample *s;
    uint32_t n, w, x, i;
    int32_t last, lo, hi;

    fw_framerect(bmp, x0, YB(ybot), x1, YB(ytop), 1);
    if (si >= st_sample_count)
        return;
    s = &st_samples[si];
    n = s->frames;
    w = (uint32_t)(x1 - x0 - 1);
    if (!n || w < 2)
        return;
    last = (int32_t)(n - 1);
    if (last < 1)
        return;
    for (x = 0; x < w; x++) {
        uint32_t a = x * n / w;             /* < 128 * 65536 */
        uint32_t b = (x + 1) * n / w;
        int32_t mn = 32767, mx = -32768;
        if (b <= a)
            b = a + 1;
        if (b > n)
            b = n;
        for (i = a; i < b; i++) {
            int32_t v = s->pcm[i];
            if (v < mn)
                mn = v;
            if (v > mx)
                mx = v;
        }
        lo = ybot - 1 - (((mx + 32768) * (ybot - ytop - 2)) >> 16);
        hi = ybot - 1 - (((mn + 32768) * (ybot - ytop - 2)) >> 16);
        if (hi < lo) {
            int32_t t = lo;
            lo = hi;
            hi = t;
        }
        if (hi > lo)
            fw_fillrect(bmp, x0 + 1 + (int32_t)x, YB(hi), x0 + 1 + (int32_t)x, YB(lo), 1);
        else
            fw_fillrect(bmp, x0 + 1 + (int32_t)x, YB(lo), x0 + 1 + (int32_t)x, YB(lo), 1);
    }
    {
        int32_t xs = x0 + 1 + start * (x1 - x0 - 1) / last;
        int32_t xe = x0 + 1 + (end > last ? last : end) * (x1 - x0 - 1) / last;
        if (xs > x0 && xs < x1)
            fw_fillrect(bmp, xs, YB(ybot - 3), xs, YB(ytop + 1), 1);
        if (xe > x0 && xe < x1)
            fw_fillrect(bmp, xe, YB(ybot - 4), xe, YB(ytop + 2), 1);
    }
}

static void draw_page(void *bmp, uint8_t *kit, int32_t track, int32_t page,
                      int32_t sel)
{
    int32_t vals[DNM_KNOBS], colw, rowh, r, c;
    int32_t base = (page - 1) * 8;
    uint32_t si = 0, locked = 0;

    shown_state(kit, track, vals, &locked);
    colw = (CX1 - CX0 + 1) / COLS;
    rowh = (CY1 - CY0 + 1) / ROWS;

    fw_fillrect(bmp, CXC, YB(CY1), CX1, YB(CY0), 0);

    {
        uint32_t n = st_sample_count;
        int32_t pick = vals[8] >> 8;
        si = n ? (uint32_t)(pick * (int32_t)n) / 128u : 0;
        if (si >= n)
            si = n ? n - 1 : 0;
    }

    /* The waveform fills the top-right three cells on both pages, with the
     * start/end the cells show (a held step's locks included). */
    if (si < st_sample_count) {
        const struct st_sample *s = &st_samples[si];
        int32_t last = (int32_t)(s->frames ? s->frames - 1 : 0);
        int32_t st = last * (vals[5] >> 8) / 127;
        int32_t en = last * (vals[6] >> 8) / 127;
        draw_wave(bmp, CX0 + colw, CY0, CX1, CY0 + rowh - 3, si, st, en);
    }

    for (r = 0; r < ROWS; r++) {
        for (c = 0; c < COLS; c++) {
            int32_t k = base + r * COLS + c;
            int32_t x = CX0 + c * colw;
            int32_t yt = CY0 + r * rowh;
            if (r == 0 && c > 0)
                continue;               /* waveform */
            if (page == 2 && k != 8 && k != 15)
                continue;               /* SYN2: SAMP top-left, CRSH bottom- */
                                        /* right; the name block takes row 1's */
                                        /* left half, so 12-14 stay clear */
            draw_cell(bmp, k, x, yt, colw - 2, vals[k], k - base == sel,
                      (int32_t)((locked >> k) & 1u));
        }
    }

    if (page == 2 && si < st_sample_count) {
        const struct st_sample *s = &st_samples[si];
        char line[24];
        int32_t j = 0, n = 0;
        const char *t = s->title;
        while (t[j] && n < 14)
            line[n++] = t[j++];
        line[n] = 0;
        fw_textf(bmp, fw_font5, CX0 + 1, YB(CY0 + rowh + 8) - 7, -1, "%s", line);
        fw_textf(bmp, fw_font5, CX0 + 1, YB(CY0 + rowh + 15) - 7, -1,
                 "%uKHZ %uMS", (unsigned)(s->rate / 1000),
                 (unsigned)((s->frames * 1000u) / (s->rate ? s->rate : 1)));
    }
}

void st_draw(void *bmp, void *ctrl)
{
    int32_t track = fw_active_track, mid, page, pos;
    uint8_t *kit;
    void *bottom, *top, *mgr, *view;
    int32_t key;
    (void)ctrl;
    st_frames++;
    if (!st_brain)
        return;
    bottom = *(void *volatile *)((uint8_t *)st_brain + BRAIN_BOTTOM);
    top = *(void *volatile *)((uint8_t *)st_brain + BRAIN_TOP);
    if (!bottom || bottom != top)
        return;
    if (track < 0 || track > 3)
        return;
    kit = fw_kit;
    if (!kit)
        return;
    mid = FW_SOUND_SLOT(kit, track, 0) >> 8;
    if (mid != ST_ID)
        return;
    key = *(volatile int32_t *)KEYVAR_AT;
    if (key != 1 && key != 2)
        return;
    page = key;
    mgr = *(void *volatile *)((uint8_t *)bottom + SCREEN_MGR);
    if (!mgr)
        return;
    view = *(void *volatile *)((uint8_t *)mgr + MGR_VIEW);
    if (!view)
        return;
    pos = *(volatile int32_t *)((uint8_t *)view + VIEW_CUR);
    if (pos != 0)
        return;
    if (msg_suppressed())
        return;
    draw_page(bmp, kit, track, page,
              st_enc_page == page && st_ticks - st_enc_at < 45 ? st_enc : -1);
}

int st_key_handler(void *brain, void *ev)
{
    int32_t id, flags, down, chord, edit;
    if (brain)
        st_brain = brain;
    if (!ev)
        return 0;
    id = *(volatile int32_t *)((const uint8_t *)ev + 12);
    flags = *(volatile int32_t *)((const uint8_t *)ev + 16);
    down = (flags & 0x01) && !(flags & 0x10);
    chord = flags & 0x02;
    /* The record mode as this key arrived: PLAY and STOP below change it, and a
     * chord is judged in the mode it was pressed in. */
    edit = (int32_t)(st_edit_rec || st_live);

    if (id == 4 || id == 5) {
        uint32_t bit = id == 4 ? 1u : 2u;
        if (down)
            st_menu |= bit;
        else
            st_menu &= ~bit;
    }
    if (id == 10) {
        if (!down) {
            st_rec_down = 0;
        } else {
            st_rec_down = 1;
            /* A bare RECORD press toggles GRID record (from LIVE it goes to
             * GRID, out of STEP it lands in GRID too); a chorded one is a copy
             * command or LIVE/STEP entry below. Exact == 1 so hold repeats do
             * not retoggle after a modal clears mid-hold. */
            if (flags == 1 && !st_trigs && !st_tracks && !st_menu) {
                if (st_live) {
                    st_live = 0;
                    st_edit_rec = 1;
                } else {
                    st_edit_rec ^= 1;
                }
            }
        }
    } else if (id == 11 && flags == 1) {
        if (st_rec_down) {
            st_live = 1;           /* RECORD+PLAY -> LIVE */
            st_edit_rec = 0;
        } else if (st_live) {
            st_live = 0;           /* PLAY exits LIVE */
        }
    } else if (id == 12 && flags == 1) {
        if (st_rec_down) {
            st_live = 0;           /* RECORD+STOP -> STEP */
            st_edit_rec = 1;
        } else if (st_live) {
            st_live = 0;           /* STOP exits LIVE */
        }
    }
    if (id >= 26 && id <= 41) {
        if (down)
            st_trigs |= 1u << (uint32_t)(id - 26);
        else
            st_trigs &= ~(1u << (uint32_t)(id - 26));
    } else if (id >= 20 && id <= 25 && (flags & 1)) {
        st_trigs = 0;              /* a page key re-points the trig row */
    }
    if (id >= 42 && id <= 45) {
        uint32_t bit = 1u << (uint32_t)(id - 42);
        if (down) {
            /* A track press switches track, re-points the page or opens the Mod
             * Menu, and the menu can eat the release of anything held: forget
             * what was held, then keep this key as the chord's modifier. */
            st_trigs = 0;
            st_rec_down = 0;
            st_menu = 0;
            st_tracks |= bit;
        } else {
            st_tracks &= ~bit;
        }
    }
    if (st_popup_chord(id, down, chord, (int32_t)st_trigs,
                        (int32_t)st_tracks, edit))
        st_msg_until = st_ticks + ST_MSG_HOLD;
    return 0;
}

int st_enc_handler(void *brain, void *ev)
{
    int32_t enc, key;
    if (brain)
        st_brain = brain;
    if (!ev)
        return 0;
    enc = *(volatile int32_t *)((const uint8_t *)ev + 12);
    if (enc < 1 || enc > 8)
        return 0;
    key = *(volatile int32_t *)KEYVAR_AT;
    if (key != 1 && key != 2)
        return 0;
    st_enc = enc - 1;
    st_enc_page = key;
    st_enc_at = st_ticks;
    return 0;
}

/* Change signature: re-arm the frame only when something the overlay shows
 * changed. Re-arming every tick starves UI handling on hardware. */
static int32_t tick_valid;
static int32_t tick_track = -1;
static int32_t tick_key = -1;
static int32_t tick_vals[DNM_KNOBS];
static int32_t tick_locked;
static int32_t tick_step = -1;
static int32_t tick_enc = -1;
static int32_t tick_enc_page = -1;
static int32_t tick_hl;

void st_tick(void *ctrl)
{
    int32_t track = fw_active_track, mid, key, page, i, hl, dirty = 0;
    int32_t vals[DNM_KNOBS], step;
    uint32_t locked = 0;
    uint8_t *kit;

    st_ticks++;
    /* The PTN and BANK popups stay up while their key is held, so hold the
     * frame back for as long as they are, and for the usual tail after. */
    if (st_menu)
        st_msg_until = st_ticks + ST_MSG_HOLD;
    if (!ctrl)
        return;
    /* Edge states must be swept on every tick, not only on dirty draws: a modal
     * can eat a release and a stuck bit would suppress the overlay forever. */
    if (st_brain) {
        void *bottom = *(void *volatile *)((uint8_t *)st_brain + BRAIN_BOTTOM);
        void *top = *(void *volatile *)((uint8_t *)st_brain + BRAIN_TOP);
        if (!bottom || bottom != top) {
            st_trigs = 0;
            st_tracks = 0;
            st_menu = 0;
            st_rec_down = 0;
        }
    }
    if (track < 0 || track > 3)
        return;
    kit = fw_kit;
    if (!kit)
        return;
    mid = FW_SOUND_SLOT(kit, track, 0) >> 8;
    keep_defaults(mid);
    if (mid != ST_ID) {
        tick_valid = 0;
        return;
    }
    key = *(volatile int32_t *)KEYVAR_AT;
    if (key != 1 && key != 2) {
        tick_valid = 0;
        return;
    }
    if (msg_suppressed()) {
        tick_valid = 0;
        return;
    }
    page = key;
    /* The same values the draw will use: a held step's locks included, so a
     * p-lock edit and the locked shading both re-arm the frame. */
    shown_state(kit, track, vals, &locked);
    step = shown_step();
    hl = st_enc >= 0 && st_enc_page == page && st_ticks - st_enc_at < 45;
    if (!tick_valid || track != tick_track || key != tick_key ||
        st_enc != tick_enc || st_enc_page != tick_enc_page || hl != tick_hl ||
        (int32_t)locked != tick_locked || step != tick_step)
        dirty = 1;
    else {
        for (i = 0; i < DNM_KNOBS; i++) {
            if (vals[i] != tick_vals[i]) {
                dirty = 1;
                break;
            }
        }
    }
    if (!dirty)
        return;
    tick_valid = 1;
    tick_track = track;
    tick_key = key;
    tick_locked = (int32_t)locked;
    tick_step = step;
    for (i = 0; i < DNM_KNOBS; i++)
        tick_vals[i] = vals[i];
    tick_enc = st_enc;
    tick_enc_page = st_enc_page;
    tick_hl = hl;
    *((volatile uint8_t *)ctrl + 0x20) = 1;
}
