/*
  Hatari - monitorframe.c

  This file is distributed under the GNU General Public License, version 2
  or at your option any later version. Read the file gpl.txt for details.

  Draw the front of an Atari SC1224 monitor - or with the monochrome
  monitor type, its name plate saying SM124 - into the padding that
  whole-pixel scaling leaves around the emulation screen, instead of
  plain black.  With black padding a dark picture, or borders that did
  not open, cannot be told apart from the window around them.

  The bezel is sized from the Atari display area, the way a real monitor
  is sized from its tube, never from the padding: the window crops it
  rather than squeezing it, so a narrow padding shows only the inner
  edge of the bezel instead of a whole bezel shrunk to a thin line.

  Proportions and shades are taken from a photo of a real SC1224, which
  was taken from slightly above: the top wall of the tube opening shows
  deeper and darker than the bottom one, and that is kept, as it is
  what makes the bezel read as an SC1224.
*/
const char MonitorFrame_fileid[] = "Hatari monitorframe.c";

#include <SDL.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "main.h"
#include "configuration.h"
#include "log.h"
#include "screen.h"
#include "monitorframe.h"

/* The nominal tube against the Atari display: a full overscan screen */
#define TUBE_SCALE_W	1.30f
#define TUBE_SCALE_H	1.38f

/* Horizontal sizes are in units of the tube width (ux), vertical ones
 * and radii in units of the tube height (uy), so the monitor stretches
 * along with the Atari picture whatever its pixel aspect.
 */
#define GLASS_RADIUS	0.040f	/* rounded corners of the tube glass */
#define RIM		0.012f	/* black band between glass and bezel */
#define OPEN_RADIUS	0.016f	/* inner edge of the bezel wall */
#define WALL_SIDE	0.026f	/* steep wall from the rim out to the face */
#define WALL_TOP	0.035f
#define WALL_BOTTOM	0.016f
#define WALL_RADIUS	0.024f	/* its rounded outer lip */
#define WALL_STEP	0.40f	/* where the outer bevel turns into the wall */
#define FACE_SIDE	0.163f	/* flat front face */
#define FACE_TOP	0.200f
#define FACE_BOTTOM	0.248f
#define CASE_RADIUS_TOP	0.085f
#define CASE_RADIUS_BOT	0.050f
#define CASE_EDGE	0.008f	/* case sides curving away at the edge */
#define PLINTH_INSET	0.128f	/* stand under the case, narrower */
#define PLINTH_HEIGHT	0.120f
#define LED_RIGHT	0.062f	/* power LED: centre from the glass corner */
#define LED_BELOW	0.149f
#define LED_W		0.023f
#define LED_H		0.012f
#define BADGE_H		0.0735f	/* name plate, below the tube on the left */
#define BADGE_BELOW	0.077f	/* its top edge, below the opening */
#define BADGE_LEFT	0.035f	/* its left end at mid height, left of it */

/* shades against the face colour: outer bevel, then inner wall */
#define SHADE_SIDE	0.91f, 0.58f
#define SHADE_TOP	0.75f, 0.38f
#define SHADE_BOTTOM	0.95f, 0.78f

#define GLASS_RGB	0x12, 0x1c, 0x18	/* unlit tube, dark green-grey */
#define RIM_RGB		0x03, 0x03, 0x03
#define LED_RGB		0x22, 0x96, 0x4e
#define OUTSIDE_RGB	0x0a, 0x0a, 0x0a	/* beyond the monitor */
#define BADGE_BLUE	0x00, 0x52, 0x9b
#define BADGE_SILVER	0xdc, 0xdd, 0xda

/* rounded rectangle, with its own radius for the top and bottom corners */
typedef struct
{
	float x0, y0, x1, y1, rt, rb;
} rrect_t;

/*
 * The name plate lettering: outlines taken from a font for the letters
 * and the Fuji, strokes along lines and circular arcs for the figures,
 * which the font has differently from the plate.
 *
 * Coordinates are in units of the cap height, y down from the top of
 * the capitals.  Arc angles are in degrees, clockwise from the right
 * since y is down, and run from a0 up to a1.
 */
typedef struct
{
	enum { LINE, ARC } type;
	float a, b, c, d, e;	/* x0,y0,x1,y1 or cx,cy,r,a0,a1 */
} stroke_t;

/* outline glyph: where its points and contours start in the run's
 * tables, and its left and right ink edges in the run */
typedef struct
{
	unsigned short point, contour, contours;
	float left, right;
} logo_glyph_t;

#include "monitorframe_logo.h"

/* stroked glyph */
typedef struct
{
	const stroke_t *strokes;
	int count;
	float left, right;	/* ink edges in the run */
} glyph_t;

/* raised print on the plate: its colour, how much it darkens from top
 * to bottom, and how strongly its edges catch the light */
typedef struct
{
	Uint8 rgb[3];
	float sheen, bevel;
} finish_t;

static const finish_t SILVER = { { BADGE_SILVER }, 0.10f, 0.16f };	/* reflective */
static const finish_t BLUE = { { BADGE_BLUE }, 0, 0.22f };		/* matte */

/* a run of either stroked glyphs or outline glyphs */
typedef struct
{
	const glyph_t *glyphs;
	const logo_glyph_t *outlines;
	const float (*points)[2];
	const unsigned short *contours;
	int count;
	float x, y, cap;	/* left, top and cap height, in plate heights */
	float weight;		/* stroke width, or how much heavier to
				 * make the outlines, in cap heights */
	const finish_t *finish;
} textrun_t;

#define NUM(a)		(int)(sizeof(a) / sizeof(a[0]))
#define STROKED(g)	g, NULL, NULL, NULL, NUM(g)
#define OUTLINED(o)	NULL, o##_glyphs, o##_points, o##_contours, NUM(o##_glyphs)

/* figures as on the plate: a flag on the 1, the 4 open at the top */
static const stroke_t glyph_1[] = {
	{ LINE, 0.25f, 0.06f, 0.25f, 0.94f },
	{ LINE, 0.25f, 0.06f, 0.08f, 0.20f },
};
static const stroke_t glyph_2[] = {
	{ ARC, 0.29f, 0.29f, 0.23f, 200.0f, 390.0f },
	{ LINE, 0.489f, 0.405f, 0.06f, 0.94f },
	{ LINE, 0.06f, 0.94f, 0.54f, 0.94f },
};
static const stroke_t glyph_4[] = {
	{ LINE, 0.16f, 0.06f, 0.07f, 0.66f },
	{ LINE, 0.07f, 0.66f, 0.56f, 0.66f },
	{ LINE, 0.46f, 0.06f, 0.46f, 0.94f },
};
#define FIGURE(g, x, w)	{ g, NUM(g), x, x + w }
static const glyph_t text_1224[] = {
	FIGURE(glyph_1, 0, 0.40f), FIGURE(glyph_2, 0.42f, 0.60f),
	FIGURE(glyph_2, 1.03f, 0.60f), FIGURE(glyph_4, 1.64f, 0.65f),
};
static const glyph_t text_124[] = {
	FIGURE(glyph_1, 0, 0.40f), FIGURE(glyph_2, 0.42f, 0.60f),
	FIGURE(glyph_4, 1.03f, 0.65f),
};

/* placed as on the plate of a real SC1224, the Fuji and SC1224 in
 * reflective silver, ATARI in matte blue */
static const textrun_t badgeColor[] = {
	{ OUTLINED(logo_fuji),  0.23f, 0.25f, 0.60f, 0.01f, &SILVER },
	{ OUTLINED(logo_atari), 1.17f, 0.25f, 0.60f, 0.02f, &BLUE },
	/* heavier on the plate than even the boldest cut of the font */
	{ OUTLINED(logo_sc),    5.68f, 0.25f, 0.60f, 0.05f, &SILVER },
	{ STROKED(text_1224),   6.66f, 0.25f, 0.60f, 0.17f, &SILVER },
};
/* the same plate for the monochrome SM124, its name ending where
 * SC1224 does */
static const textrun_t badgeMono[] = {
	{ OUTLINED(logo_fuji),  0.23f, 0.25f, 0.60f, 0.01f, &SILVER },
	{ OUTLINED(logo_atari), 1.17f, 0.25f, 0.60f, 0.02f, &BLUE },
	{ OUTLINED(logo_sm),    5.83f, 0.25f, 0.60f, 0.05f, &SILVER },
	{ STROKED(text_124),    7.03f, 0.25f, 0.60f, 0.17f, &SILVER },
};
#define BADGE_W		8.57f	/* plate length, in plate heights */
#define BADGE_SLANT	1.04f	/* its ends: sideways shift per height */

/* a text run as laid out on the pixel grid */
typedef struct
{
	const textrun_t *run;
	float x, y, cap;	/* in pixels from the plate's corner */
} layout_t;

/* where everything goes, in pixels from the emulation screen's corner */
typedef struct
{
	float ux, uy;			/* tube width and height */
	bool mono;			/* an SM124 rather than an SC1224 */
	int screen_w, screen_h;
	rrect_t tube, open, lip, body, plinth, lamp, badge;
	float badge_h, badge_x;
	SDL_Rect area;			/* all of the monitor */
} geom_t;

/*
 * The monitor, drawn once for each size of the screen in the window.
 * With the screen scaled by a whole number, resizing the window only
 * steps through a few of those, so each is kept: resizing then just
 * picks one and crops it, also when going back and forth across a
 * step, instead of drawing the monitor again in the middle of it.
 */
#define FRAME_CACHE	8

typedef struct
{
	SDL_Texture *texture;
	SDL_Rect area;		/* the monitor, from the screen's corner */
	/* what it was drawn for */
	int screen_w, screen_h;
	int display_w, display_h;
	bool mono;
	SDL_Rect atari;
	Uint32 rgb;
	unsigned int used;	/* when last used, to replace the oldest */
} frame_t;

static frame_t frameCache[FRAME_CACHE];
static unsigned int frameClock;


/**
 * Parse the configured case colour, falling back to the default when
 * it is not of the #RRGGBB form.
 */
static Uint32 MonitorFrame_GetColor(void)
{
	const char *color = ConfigureParams.Screen.szMonitorFrameColor;
	unsigned int rgb;
	int len = 0;

	if (sscanf(color, "#%6x%n", &rgb, &len) != 1 || len != 7 || color[len])
	{
		Log_Printf(LOG_WARN, "Monitor frame color '%s' is not #RRGGBB, using %s\n",
			   color, SCREEN_FRAME_COLOR_DEFAULT);
		sscanf(SCREEN_FRAME_COLOR_DEFAULT, "#%6x", &rgb);
		/* warn once, not every time the frame is redrawn */
		strcpy(ConfigureParams.Screen.szMonitorFrameColor, SCREEN_FRAME_COLOR_DEFAULT);
	}
	return rgb;
}


/**
 * Signed distance from (px,py) to the edge of a rounded rectangle,
 * negative inside, and the outward direction of the nearest edge.
 */
static float MonitorFrame_Dist(const rrect_t *rr, float px, float py,
			       float *nx, float *ny)
{
	float cx = (rr->x0 + rr->x1) / 2, cy = (rr->y0 + rr->y1) / 2;
	float dx = px - cx, dy = py - cy;
	float sx = dx < 0 ? -1 : 1, sy = dy < 0 ? -1 : 1;
	float r = dy < 0 ? rr->rt : rr->rb;
	float qx = fabsf(dx) - ((rr->x1 - rr->x0) / 2 - r);
	float qy = fabsf(dy) - ((rr->y1 - rr->y0) / 2 - r);
	float len;

	if (qx > 0 && qy > 0)
	{
		len = sqrtf(qx * qx + qy * qy);
		*nx = sx * qx / len;
		*ny = sy * qy / len;
		return len - r;
	}
	if (qx > qy)
	{
		*nx = sx;
		*ny = 0;
		return qx - r;
	}
	*nx = 0;
	*ny = sy;
	return qy - r;
}

/* how much of a pixel at distance d lies inside: one pixel antialiasing */
static inline float MonitorFrame_Cover(float d)
{
	return d <= -0.5f ? 1.0f : d >= 0.5f ? 0.0f : 0.5f - d;
}

static inline float MonitorFrame_Smooth(float e0, float e1, float v)
{
	float t = (v - e0) / (e1 - e0);

	t = t < 0 ? 0 : t > 1 ? 1 : t;
	return t * t * (3 - 2 * t);
}

static inline void MonitorFrame_Mix(float *c, const float *with, float a)
{
	c[0] += (with[0] - c[0]) * a;
	c[1] += (with[1] - c[1]) * a;
	c[2] += (with[2] - c[2]) * a;
}

static inline void MonitorFrame_Shade(float *c, const float *base, float f)
{
	c[0] = base[0] * f;
	c[1] = base[1] * f;
	c[2] = base[2] * f;
}

/* with dither added, which keeps the gentle gradients from banding */
static inline Uint8 MonitorFrame_Clamp(float v, float dither)
{
	v += dither;
	return v <= 0 ? 0 : v >= 255 ? 255 : (Uint8)v;
}

/**
 * Signed distance from (x,y) to the edge of a filled outline, negative
 * inside, and the vector to (x,y) from the nearest point on the edge.
 * Font outlines do not overlap themselves, so even-odd filling gives
 * the same result as the font's own non-zero rule.
 */
static float MonitorFrame_OutlineDist(const textrun_t *run, const logo_glyph_t *g,
				     float x, float y, float *vx, float *vy)
{
	const float (*p)[2] = run->points + g->point;
	float dmin = 1e9f, dx, dy, ex, ey, t, d2;
	bool inside = false;
	int c, i, j, n;

	for (c = g->contour; c < g->contour + g->contours; p += n, c++)
	{
		n = run->contours[c];
		for (i = 0, j = n - 1; i < n; j = i++)
		{
			ex = p[i][0] - p[j][0];
			ey = p[i][1] - p[j][1];
			if ((p[i][1] > y) != (p[j][1] > y)
			    && x < p[j][0] + (y - p[j][1]) * ex / ey)
				inside = !inside;

			t = ((x - p[j][0]) * ex + (y - p[j][1]) * ey) / (ex * ex + ey * ey + 1e-12f);
			t = t < 0 ? 0 : t > 1 ? 1 : t;
			dx = x - (p[j][0] + t * ex);
			dy = y - (p[j][1] + t * ey);
			d2 = dx * dx + dy * dy;
			if (d2 < dmin)
			{
				dmin = d2;
				*vx = dx;
				*vy = dy;
			}
		}
	}
	dmin = sqrtf(dmin);
	return inside ? -dmin : dmin;
}

/**
 * Distance from (x,y) to the centre line of a stroke, and the vector to
 * (x,y) from the nearest point on it.
 */
static float MonitorFrame_StrokeDist(const stroke_t *s, float x, float y,
				     float *vx, float *vy)
{
	float dx, dy, len, t, ang, ex, ey;

	if (s->type == LINE)
	{
		dx = s->c - s->a;
		dy = s->d - s->b;
		t = ((x - s->a) * dx + (y - s->b) * dy) / (dx * dx + dy * dy);
		t = t < 0 ? 0 : t > 1 ? 1 : t;
		*vx = x - (s->a + t * dx);
		*vy = y - (s->b + t * dy);
		return sqrtf(*vx * *vx + *vy * *vy);
	}

	dx = x - s->a;
	dy = y - s->b;
	len = sqrtf(dx * dx + dy * dy);
	ang = atan2f(dy, dx) * (180.0f / (float)M_PI);
	while (ang < s->d)
		ang += 360.0f;
	while (ang >= s->d + 360.0f)
		ang -= 360.0f;
	if (ang <= s->e && len > 1e-6f)
	{
		*vx = dx * (1 - s->c / len);
		*vy = dy * (1 - s->c / len);
		return fabsf(len - s->c);
	}

	/* past either end of the arc: nearest end point */
	ex = x - (s->a + s->c * cosf(s->d * (float)M_PI / 180.0f));
	ey = y - (s->b + s->c * sinf(s->d * (float)M_PI / 180.0f));
	*vx = ex;
	*vy = ey;
	len = sqrtf(ex * ex + ey * ey);
	ex = x - (s->a + s->c * cosf(s->e * (float)M_PI / 180.0f));
	ey = y - (s->b + s->c * sinf(s->e * (float)M_PI / 180.0f));
	if (ex * ex + ey * ey < len * len)
	{
		*vx = ex;
		*vy = ey;
		len = sqrtf(ex * ex + ey * ey);
	}
	return len;
}

/**
 * Signed distance in pixels from (x,y) to the edge of the lettering of
 * a run, negative inside, and the outward direction of that edge.
 * Each glyph is moved to start on a whole pixel, which together with
 * the whole-pixel cap height and baseline keeps stems and bars sharp
 * at the small sizes the plate is drawn in.
 */
static float MonitorFrame_RunDist(const layout_t *lay, float x, float y,
				  float *nx, float *ny)
{
	const textrun_t *run = lay->run;
	float gx, gy, d, dmin = 1e9f, left, shift, vx = 0, vy = 0, sx, sy, len;
	int i, k;

	*nx = *ny = 0;
	gy = (y - lay->y) / lay->cap;
	for (i = 0; i < run->count; i++)
	{
		left = run->outlines ? run->outlines[i].left : run->glyphs[i].left;
		/* glyph's ink edge onto a pixel boundary */
		shift = lay->x + left * lay->cap;
		shift = roundf(shift) - shift;
		gx = (x - lay->x - shift) / lay->cap;
		if (gx < left - 0.5f
		    || gx > (run->outlines ? run->outlines[i].right : run->glyphs[i].right) + 0.5f)
			continue;

		if (run->outlines)
		{
			d = MonitorFrame_OutlineDist(run, &run->outlines[i], gx, gy, &vx, &vy);
			if (d < 0)
			{
				vx = -vx;
				vy = -vy;
			}
			d -= run->weight;
		}
		else
		{
			d = 1e9f;
			for (k = 0; k < run->glyphs[i].count; k++)
			{
				len = MonitorFrame_StrokeDist(&run->glyphs[i].strokes[k],
							      gx - left, gy, &sx, &sy);
				if (len < d)
				{
					d = len;
					vx = sx;
					vy = sy;
				}
			}
			d -= run->weight / 2;
		}
		if (d < dmin)
		{
			dmin = d;
			len = sqrtf(vx * vx + vy * vy);
			if (len > 1e-6f)
			{
				*nx = vx / len;
				*ny = vy / len;
			}
		}
	}
	/* cut flat at the baseline and the cap height */
	if (-gy > dmin)
	{
		dmin = -gy;
		*nx = 0;
		*ny = -1;
	}
	if (gy - 1 > dmin)
	{
		dmin = gy - 1;
		*nx = 0;
		*ny = 1;
	}
	return dmin * lay->cap;
}

/**
 * Lettering printed raised on the plate: lit from the upper left on its
 * edges, with a faint shadow on the plate below it to the right, and
 * when reflective a sheen darkening downwards.
 */
static void MonitorFrame_Print(float *c, const layout_t *lay, float x, float y)
{
	const finish_t *finish = lay->run->finish;
	const float ink[3] = { finish->rgb[0], finish->rgb[1], finish->rgb[2] };
	float d, ds, cover, col[3], nx, ny, f, lit, bevel, off;

	off = fmaxf(0.6f, 0.025f * lay->cap);
	d = MonitorFrame_RunDist(lay, x, y, &nx, &ny);
	if (d > off + 1.0f)
		return;

	cover = MonitorFrame_Cover(d);
	if (cover < 1)
	{
		/* shadow, where the lettering moved down right would cover */
		float sx, sy;

		ds = MonitorFrame_RunDist(lay, x - off, y - off, &sx, &sy);
		f = 1 - 0.07f * MonitorFrame_Cover(ds) * (1 - cover);
		c[0] *= f; c[1] *= f; c[2] *= f;
		if (cover <= 0)
			return;
	}

	f = 1 + finish->sheen * (0.5f - fminf(fmaxf((y - lay->y) / lay->cap, 0), 1));
	MonitorFrame_Shade(col, ink, f);

	bevel = fmaxf(0.7f, 0.06f * lay->cap);
	if (-d < bevel + 0.5f)
	{
		/* lit edges go towards white, the others darker, so dark
		 * ink shows its relief as well as light */
		lit = finish->bevel * (-0.6f * nx - 0.8f * ny)
			* (1 - MonitorFrame_Smooth(0, bevel, -d));
		if (lit > 0)
			MonitorFrame_Mix(col, (const float[3]){ 255, 255, 255 }, lit);
		else
			MonitorFrame_Shade(col, col, 1 + lit);
	}
	MonitorFrame_Mix(c, col, cover);
}

/**
 * Draw the name plate pixel at (x,y), in pixels from the top of the
 * middle of its left end, over colour c. 'h' is the plate height in
 * pixels.
 */
static void MonitorFrame_Badge(float *c, const float *base, float x, float y, float h,
			       bool mono)
{
	const textrun_t *text = mono ? badgeMono : badgeColor;
	const int count = mono ? NUM(badgeMono) : NUM(badgeColor);
	const float norm = 1.0f / sqrtf(1 + BADGE_SLANT * BADGE_SLANT);
	float u = x / h, v = y / h, d, col[3];
	layout_t lay;
	int i;

	/* parallelogram, ends sloping up to the right */
	d = fmaxf(-v, v - 1);
	d = fmaxf(d, ((0.5f - v) * BADGE_SLANT - u) * norm);
	d = fmaxf(d, (u - BADGE_W - (0.5f - v) * BADGE_SLANT) * norm);
	d *= h;
	if (d >= 0.5f)
		return;

	MonitorFrame_Shade(col, base, 1.05f);
	MonitorFrame_Mix(c, col, MonitorFrame_Cover(d));
	/* thin dark outline round its edge */
	MonitorFrame_Shade(col, base, 0.65f);
	MonitorFrame_Mix(c, col, MonitorFrame_Cover(d) *
			 (1 - MonitorFrame_Smooth(0, fmaxf(1.0f, 0.015f * h), -d)));

	for (i = 0; i < count; i++)
	{
		/* top and cap height on whole pixels */
		lay.run = &text[i];
		lay.x = lay.run->x * h;
		lay.y = roundf(lay.run->y * h);
		lay.cap = fmaxf(roundf(lay.run->cap * h), 1);
		if (y < lay.y - lay.cap * 0.5f || y > lay.y + lay.cap * 1.5f)
			continue;

		MonitorFrame_Print(c, &lay, x, y);
	}
}

static void MonitorFrame_Grow(rrect_t *r, const rrect_t *from, float left,
			      float top, float right, float bottom)
{
	r->x0 = from->x0 - left;
	r->y0 = from->y0 - top;
	r->x1 = from->x1 + right;
	r->y1 = from->y1 + bottom;
}


/**
 * Lay out the monitor around a screen_w x screen_h emulation screen with
 * the Atari display at 'atari' in it, in pixels from the screen's corner.
 * Its area is limited to what a window of up to display_w x display_h,
 * with the screen in its middle, can show.
 */
static void MonitorFrame_Layout(geom_t *g, int screen_w, int screen_h,
				int display_w, int display_h, bool mono,
				const SDL_Rect *atari)
{
	float ux, uy, cx, cy, margin;
	SDL_Rect view;

	g->mono = mono;
	g->screen_w = screen_w;
	g->screen_h = screen_h;

	/* the nominal tube, centered on the Atari display */
	g->ux = ux = atari->w * TUBE_SCALE_W;
	g->uy = uy = atari->h * TUBE_SCALE_H;
	cx = atari->x + atari->w / 2.0f;
	cy = atari->y + atari->h / 2.0f;

	/* the glass holds the tube, and the whole emulation screen when
	 * that is larger - with enough room that its rounded corners
	 * clear the screen's own square corners
	 */
	g->tube.rt = g->tube.rb = GLASS_RADIUS * uy;
	margin = g->tube.rt * 0.3f;
	g->tube.x0 = fminf(cx - ux / 2, -margin);
	g->tube.y0 = fminf(cy - uy / 2, -margin);
	g->tube.x1 = fmaxf(cx + ux / 2, screen_w + margin);
	g->tube.y1 = fmaxf(cy + uy / 2, screen_h + margin);

	MonitorFrame_Grow(&g->open, &g->tube, RIM * uy, RIM * uy, RIM * uy, RIM * uy);
	g->open.rt = g->open.rb = OPEN_RADIUS * uy;
	MonitorFrame_Grow(&g->lip, &g->open, WALL_SIDE * ux, WALL_TOP * uy,
			  WALL_SIDE * ux, WALL_BOTTOM * uy);
	g->lip.rt = g->lip.rb = WALL_RADIUS * uy;
	MonitorFrame_Grow(&g->body, &g->lip, FACE_SIDE * ux, FACE_TOP * uy,
			  FACE_SIDE * ux, FACE_BOTTOM * uy);
	g->body.rt = CASE_RADIUS_TOP * uy;
	g->body.rb = CASE_RADIUS_BOT * uy;

	g->plinth.x0 = g->body.x0 + PLINTH_INSET * ux;
	g->plinth.x1 = g->body.x1 - PLINTH_INSET * ux;
	g->plinth.y0 = g->body.y1 - CASE_RADIUS_BOT * uy;
	g->plinth.y1 = g->body.y1 + PLINTH_HEIGHT * uy;
	g->plinth.rt = g->plinth.rb = 0;

	g->lamp.x0 = g->tube.x1 - (LED_RIGHT + LED_W / 2) * ux;
	g->lamp.x1 = g->lamp.x0 + LED_W * ux;
	g->lamp.y0 = g->tube.y1 + (LED_BELOW - LED_H / 2) * uy;
	g->lamp.y1 = g->lamp.y0 + LED_H * uy;
	g->lamp.rt = g->lamp.rb = LED_H * uy * 0.2f;

	/* name plate: positioned against the tube, but with its lettering
	 * kept in proportion rather than stretched along with the picture
	 */
	g->badge_h = fmaxf(roundf(BADGE_H * uy), 1);
	g->badge_x = roundf(g->open.x0 - BADGE_LEFT * ux);
	g->badge.y0 = roundf(g->open.y1 + BADGE_BELOW * uy);
	g->badge.y1 = g->badge.y0 + g->badge_h;
	g->badge.x0 = g->badge_x - BADGE_SLANT * g->badge_h / 2 - 1;
	g->badge.x1 = g->badge_x + (BADGE_W + BADGE_SLANT / 2) * g->badge_h + 1;

	/* with a pixel to spare for the antialiased edges */
	g->area.x = (int)(g->body.x0 - 1.0f) - 1;
	g->area.y = (int)(g->body.y0 - 1.0f) - 1;
	g->area.w = (int)(g->body.x1 + 1.0f) + 1 - g->area.x;
	g->area.h = (int)(g->plinth.y1 + 1.0f) + 1 - g->area.y;

	/* no further out than the largest window shows, on either side
	 * of the centered screen, with the odd pixel of centering */
	view.x = -((display_w - screen_w + 1) / 2) - 1;
	view.y = -((display_h - screen_h + 1) / 2) - 1;
	view.w = display_w + 2;
	view.h = display_h + 2;
	if (view.x > -1)
	{
		view.x = -1;
		view.w = screen_w + 2;
	}
	if (view.y > -1)
	{
		view.y = -1;
		view.h = screen_h + 2;
	}
	SDL_IntersectRect(&g->area, &view, &g->area);
}


/**
 * Colour of the monitor at pixel centre (px,py).
 */
static Uint32 MonitorFrame_Pixel(const geom_t *g, const float *base, float px, float py,
				 float dither)
{
	const float glass[3] = { GLASS_RGB }, rim[3] = { RIM_RGB };
	const float led[3] = { LED_RGB }, outside[3] = { OUTSIDE_RGB };
	const float side[2] = { SHADE_SIDE }, top[2] = { SHADE_TOP };
	const float bottom[2] = { SHADE_BOTTOM };
	float c[3], col[3], nx, ny, d, d_body, d_lip, d_open, f, t, wx, wy;
	const float *vert;

	c[0] = outside[0]; c[1] = outside[1]; c[2] = outside[2];

	/* the case front, a little lighter at the top, its edges curving
	 * away out of the light; and where it does not fully cover, the
	 * stand under it, in its shadow
	 */
	d_body = MonitorFrame_Dist(&g->body, px, py, &nx, &ny);
	if (d_body > -0.5f)
	{
		d = MonitorFrame_Dist(&g->plinth, px, py, &nx, &ny);
		if (d < 0.5f)
		{
			t = (py - g->body.y1) / (PLINTH_HEIGHT * g->uy);
			MonitorFrame_Shade(col, base, 0.46f + 0.29f * MonitorFrame_Smooth(0, 1, t));
			MonitorFrame_Mix(c, col, MonitorFrame_Cover(d));
		}
	}
	if (d_body < 0.5f)
	{
		f = 1.05f - 0.05f * (py - g->body.y0) / (g->body.y1 - g->body.y0);
		f *= 0.82f + 0.18f * MonitorFrame_Smooth(0, CASE_EDGE * g->ux, -d_body);
		MonitorFrame_Shade(col, base, f);
		MonitorFrame_Mix(c, col, MonitorFrame_Cover(d_body));

		if (py > g->badge.y0 - 1 && py < g->badge.y1 + 1
		    && px > g->badge.x0 && px < g->badge.x1)
			MonitorFrame_Badge(c, base, px - g->badge_x,
					   py - g->badge.y0, g->badge_h, g->mono);

		if (px > g->lamp.x0 - 1 && px < g->lamp.x1 + 1
		    && py > g->lamp.y0 - 1 && py < g->lamp.y1 + 1)
		{
			d = MonitorFrame_Dist(&g->lamp, px, py, &nx, &ny);
			MonitorFrame_Mix(c, led, MonitorFrame_Cover(d));
		}
	}

	/* wall into the tube opening: a lighter bevel off the face, then
	 * the steep wall proper, each shaded by which way it faces
	 */
	if (px > g->lip.x0 - 1 && px < g->lip.x1 + 1
	    && py > g->lip.y0 - 1 && py < g->lip.y1 + 1
	    && (d_lip = MonitorFrame_Dist(&g->lip, px, py, &nx, &ny)) < 0.5f)
	{
		d_open = MonitorFrame_Dist(&g->open, px, py, &nx, &ny);
		/* 0 at the rim, 1 at the face */
		t = d_open > 0 ? d_open / (d_open - fminf(d_lip, 0) + 1e-3f) : 0;
		wx = nx * nx;
		wy = ny * ny;
		vert = ny < 0 ? top : bottom;
		f = MonitorFrame_Smooth(WALL_STEP - 0.08f, WALL_STEP + 0.08f, t);
		f = wx * (side[1] + (side[0] - side[1]) * f)
		  + wy * (vert[1] + (vert[0] - vert[1]) * f);
		MonitorFrame_Shade(col, base, f / (wx + wy));
		MonitorFrame_Mix(c, col, MonitorFrame_Cover(d_lip));

		/* black band round the tube, then the glass */
		MonitorFrame_Mix(c, rim, MonitorFrame_Cover(d_open));
		d = MonitorFrame_Dist(&g->tube, px, py, &nx, &ny);
		MonitorFrame_Mix(c, glass, MonitorFrame_Cover(d));
	}

	return 0xff000000u
		| (Uint32)MonitorFrame_Clamp(c[0], dither) << 16
		| (Uint32)MonitorFrame_Clamp(c[1], dither) << 8
		| MonitorFrame_Clamp(c[2], dither);
}


/**
 * Draw the monitor into 'pixels', g->area large.  Pixels the emulation
 * screen covers anyway are left out.
 */
static void MonitorFrame_Render(const geom_t *g, Uint32 *pixels, Uint32 rgb)
{
	/* 2x2 ordered dither, which keeps the gentle gradients from banding */
	static const float bayer[4] = { 0.125f, 0.625f, 0.875f, 0.375f };
	const float base[3] = { (rgb >> 16) & 0xff, (rgb >> 8) & 0xff, rgb & 0xff };
	int x, y, sx0, sx1;

	/* the screen's columns inside the area */
	sx0 = -g->area.x;
	sx1 = sx0 + g->screen_w;

	for (y = 0; y < g->area.h; y++)
	{
		Uint32 *row = pixels + (size_t)y * g->area.w;
		float py = g->area.y + y + 0.5f;
		bool under = py > 0 && py < g->screen_h;

		for (x = 0; x < g->area.w; x++)
		{
			if (under && x == sx0)
			{
				x = sx1 - 1;
				continue;
			}
			row[x] = MonitorFrame_Pixel(g, base, g->area.x + x + 0.5f, py,
						    bayer[(y & 1) * 2 + (x & 1)]);
		}
	}
}


/**
 * Copy the part of the monitor texture that is at window rectangle
 * 'part', as far as the window shows it.
 */
static void MonitorFrame_Blit(SDL_Renderer *renderer, SDL_Texture *texture,
			      const SDL_Rect *window, const SDL_Rect *monitor, SDL_Rect part)
{
	SDL_Rect src;

	if (!SDL_IntersectRect(&part, window, &part)
	    || !SDL_IntersectRect(&part, monitor, &part))
		return;
	src.x = part.x - monitor->x;
	src.y = part.y - monitor->y;
	src.w = part.w;
	src.h = part.h;
	SDL_RenderCopy(renderer, texture, &src, &part);
}


/**
 * The monitor for this screen size, Atari display and colour: from the
 * cache, or drawn now in place of the one least recently used.
 */
static frame_t *MonitorFrame_Get(SDL_Renderer *renderer, int screen_w, int screen_h,
				 int display_w, int display_h, bool mono,
				 const SDL_Rect *atari, Uint32 rgb)
{
	frame_t *f, *oldest = &frameCache[0];
	geom_t geom;
	Uint32 *pixels;
	int i;

	for (i = 0; i < FRAME_CACHE; i++)
	{
		f = &frameCache[i];
		if (f->texture && f->screen_w == screen_w && f->screen_h == screen_h
		    && f->display_w == display_w && f->display_h == display_h
		    && f->mono == mono
		    && !memcmp(&f->atari, atari, sizeof(*atari)) && f->rgb == rgb)
		{
			f->used = ++frameClock;
			return f;
		}
		if (!f->texture || f->used < oldest->used)
			oldest = f;
	}

	f = oldest;
	if (f->texture)
		SDL_DestroyTexture(f->texture);
	MonitorFrame_Layout(&geom, screen_w, screen_h, display_w, display_h, mono, atari);
	pixels = calloc((size_t)geom.area.w * geom.area.h, sizeof(*pixels));
	f->texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888,
				       SDL_TEXTUREACCESS_STATIC,
				       geom.area.w, geom.area.h);
	if (!pixels || !f->texture)
	{
		free(pixels);
		if (f->texture)
			SDL_DestroyTexture(f->texture);
		f->texture = NULL;
		return NULL;
	}
	MonitorFrame_Render(&geom, pixels, rgb);
	SDL_UpdateTexture(f->texture, NULL, pixels, geom.area.w * sizeof(*pixels));
	free(pixels);

	f->area = geom.area;
	f->screen_w = screen_w;
	f->screen_h = screen_h;
	f->display_w = display_w;
	f->display_h = display_h;
	f->mono = mono;
	f->atari = *atari;
	f->rgb = rgb;
	f->used = ++frameClock;
	return f;
}


/**
 * Draw the monitor frame over the whole render target, which has to be
 * win_w x win_h with no logical size set. 'screen' is where the
 * emulation screen goes in the window and 'atari' where the Atari
 * display is inside it, both in window pixels. display_w x display_h is
 * the largest the window can get, in the same pixels.
 *
 * This is only a crop of a monitor drawn before, unless the screen is
 * at a size in the window that it has not been at yet: each is drawn
 * as large as any window on the display can show it. Under the screen
 * the monitor is not copied, as the screen goes over it.
 */
void MonitorFrame_Draw(SDL_Renderer *renderer, int win_w, int win_h,
		       int display_w, int display_h,
		       const SDL_Rect *screen, const SDL_Rect *atari)
{
	SDL_Rect rel = { atari->x - screen->x, atari->y - screen->y, atari->w, atari->h };
	SDL_Rect dst, window, strip, outside[4];
	Uint8 r, g, b, a;
	frame_t *f;
	int n;

	if (win_w <= 0 || win_h <= 0 || atari->w <= 0 || atari->h <= 0)
		return;

	f = MonitorFrame_Get(renderer, screen->w, screen->h, display_w, display_h,
			     ConfigureParams.Screen.nMonitorType == MONITOR_TYPE_MONO,
			     &rel, MonitorFrame_GetColor());
	if (!f)
		return;

	/* the monitor in the window, and the strips of it around the
	 * screen: what is under the screen, or outside the window, is
	 * not copied at all, which matters as this happens every frame
	 */
	dst.x = screen->x + f->area.x;
	dst.y = screen->y + f->area.y;
	dst.w = f->area.w;
	dst.h = f->area.h;
	window.x = window.y = 0;
	window.w = win_w;
	window.h = win_h;

	strip.x = dst.x;
	strip.w = dst.w;
	strip.y = dst.y;
	strip.h = screen->y - dst.y;
	MonitorFrame_Blit(renderer, f->texture, &window, &dst, strip);	/* above */
	strip.y = screen->y + screen->h;
	strip.h = dst.y + dst.h - strip.y;
	MonitorFrame_Blit(renderer, f->texture, &window, &dst, strip);	/* below */
	strip.y = screen->y;
	strip.h = screen->h;
	strip.w = screen->x - dst.x;
	MonitorFrame_Blit(renderer, f->texture, &window, &dst, strip);	/* left */
	strip.x = screen->x + screen->w;
	strip.w = dst.x + dst.w - strip.x;
	MonitorFrame_Blit(renderer, f->texture, &window, &dst, strip);	/* right */

	/* the window beyond the monitor, when it is larger */
	n = 0;
	if (dst.y > 0)
		outside[n++] = (SDL_Rect){ 0, 0, win_w, dst.y };
	if (dst.y + dst.h < win_h)
		outside[n++] = (SDL_Rect){ 0, dst.y + dst.h, win_w, win_h - dst.y - dst.h };
	if (dst.x > 0)
		outside[n++] = (SDL_Rect){ 0, dst.y, dst.x, dst.h };
	if (dst.x + dst.w < win_w)
		outside[n++] = (SDL_Rect){ dst.x + dst.w, dst.y, win_w - dst.x - dst.w, dst.h };
	if (n)
	{
		SDL_GetRenderDrawColor(renderer, &r, &g, &b, &a);
		SDL_SetRenderDrawColor(renderer, OUTSIDE_RGB, 255);
		SDL_RenderFillRects(renderer, outside, n);
		SDL_SetRenderDrawColor(renderer, r, g, b, a);
	}
}


/**
 * Free the cached monitors, e.g. before the renderer they were made for
 * goes away.
 */
void MonitorFrame_Free(void)
{
	int i;

	for (i = 0; i < FRAME_CACHE; i++)
	{
		if (frameCache[i].texture)
			SDL_DestroyTexture(frameCache[i].texture);
		frameCache[i].texture = NULL;
	}
}
