/* SPDX-License-Identifier: MIT
 *
 * fbcon.c — display console for both display types firmware hands over.
 *
 * DESIGN NOTE (HavenDOS → HavenOS)
 * HavenDOS reached UEFI hardware and then had to add framebuffer
 * rendering as a separate path from its VGA text driver, ending up with
 * drivers/vga.c, drivers/vesa.c and drivers/mode13.c as three parallel
 * display implementations with three sets of cursor and scroll logic, and
 * a desktop that "may remain at the top-left" because the text layer did
 * not know the panel size.
 *
 * HavenOS has one console driver with two back ends chosen by what
 * firmware actually provided:
 *
 *   BOOT_FB_EGA_TEXT  BIOS left us an 80x25 text buffer at 0xB8000
 *   BOOT_FB_RGB       UEFI (or a VBE mode) left us a linear framebuffer
 *
 * Everything above the two blit primitives — the cell grid, the cursor,
 * scrolling, tab and newline handling, the ANSI-free escape handling — is
 * shared.  A 4K panel and an 80x25 text screen differ in exactly two
 * functions, cell_put() and cell_scroll().
 *
 * Both back ends compute the grid from the real geometry, so the console
 * fills the panel instead of sitting in a corner of it.
 */
#include <haven/console.h>
#include <haven/boot.h>
#include <haven/arch.h>
#include <haven/string.h>
#include <haven/font8x8.h>
#include <haven/fbcon.h>

/* The glyph cell.  8x8 is the font's natural size; the renderer scales it
   by an integer factor so text stays legible on a high-DPI panel instead
   of becoming unreadably small — which is the generalisation of the
   "display may remain at the top-left" limitation HavenDOS documented. */
#define CELL_W (FONT8X8_WIDTH)
#define CELL_H (FONT8X8_HEIGHT)

#define MAX_COLS 512
#define MAX_ROWS 256

enum fb_kind {
	FB_NONE = 0,
	FB_EGA_TEXT,
	FB_RGB,
};

static struct {
	enum fb_kind kind;
	volatile uint8_t *base;    /* mapped through the direct map */
	uint32_t pitch;
	uint32_t width_px, height_px;
	uint8_t  bytes_per_pixel;
	uint8_t  r_shift, g_shift, b_shift;
	uint8_t  r_bits,  g_bits,  b_bits;

	uint32_t cols, rows;       /* character grid */
	uint32_t scale;            /* integer glyph magnification */
	uint32_t origin_x, origin_y; /* centring offset in pixels */

	uint32_t cx, cy;           /* cursor, in cells */
	uint32_t fg, bg;           /* packed pixel values (RGB) or attribute */
	bool     ready;
} fb;

/* HavenOS palette, indexed like the 16 EGA colours so the EGA back end can
   use the index directly and the RGB back end can look up a real colour. */
static const uint32_t palette_rgb[16] = {
	0x000000, 0x0000AA, 0x00AA00, 0x00AAAA,
	0xAA0000, 0xAA00AA, 0xAA5500, 0xAAAAAA,
	0x555555, 0x5555FF, 0x55FF55, 0x55FFFF,
	0xFF5555, 0xFF55FF, 0xFFFF55, 0xFFFFFF,
};

#define COLOR_FG 7    /* light grey */
#define COLOR_BG 0    /* black */

/* Pack an 0xRRGGBB value into this framebuffer's pixel format.  Firmware
   does not promise BGRA: the Multiboot2 framebuffer tag reports per-channel
   shift and size, and ignoring them is how a boot ends up with a blue-on-
   orange console on hardware that reports RGB in a different order. */
static uint32_t pack_rgb(uint32_t rgb)
{
	uint32_t r = (rgb >> 16) & 0xFF;
	uint32_t g = (rgb >> 8) & 0xFF;
	uint32_t b = rgb & 0xFF;

	/* Scale each channel down to the reported width. */
	r >>= (8 - fb.r_bits);
	g >>= (8 - fb.g_bits);
	b >>= (8 - fb.b_bits);

	return (r << fb.r_shift) | (g << fb.g_shift) | (b << fb.b_shift);
}

static inline void put_pixel(uint32_t x, uint32_t y, uint32_t pixel)
{
	if (x >= fb.width_px || y >= fb.height_px)
		return;

	volatile uint8_t *p = fb.base + (size_t)y * fb.pitch +
	                      (size_t)x * fb.bytes_per_pixel;

	switch (fb.bytes_per_pixel) {
	case 4:
		*(volatile uint32_t *)p = pixel;
		break;
	case 3:
		p[0] = (uint8_t)(pixel);
		p[1] = (uint8_t)(pixel >> 8);
		p[2] = (uint8_t)(pixel >> 16);
		break;
	case 2:
		*(volatile uint16_t *)p = (uint16_t)pixel;
		break;
	default:
		*p = (uint8_t)pixel;
		break;
	}
}

/* ── EGA text back end ───────────────────────────────────────────────── */

static void ega_cell_put(uint32_t col, uint32_t row, char c)
{
	volatile uint16_t *cells = (volatile uint16_t *)fb.base;
	uint8_t  attr  = (uint8_t)((fb.bg << 4) | (fb.fg & 0x0F));
	cells[row * fb.cols + col] = (uint16_t)(((uint16_t)attr << 8) |
	                                        (uint8_t)c);
}

static void ega_scroll(void)
{
	volatile uint16_t *cells = (volatile uint16_t *)fb.base;
	size_t  row_cells = fb.cols;
	uint8_t attr      = (uint8_t)((fb.bg << 4) | (fb.fg & 0x0F));
	uint16_t blank    = (uint16_t)(((uint16_t)attr << 8) | ' ');

	for (uint32_t y = 1; y < fb.rows; y++) {
		for (uint32_t x = 0; x < fb.cols; x++)
			cells[(y - 1) * row_cells + x] = cells[y * row_cells + x];
	}
	for (uint32_t x = 0; x < fb.cols; x++)
		cells[(fb.rows - 1) * row_cells + x] = blank;
}

/* ── RGB framebuffer back end ────────────────────────────────────────── */

static void rgb_cell_put(uint32_t col, uint32_t row, char c)
{
	uint8_t glyph_index = (uint8_t)c;
	/* The font has blanks above 127; drawing them as spaces keeps a
	   stray high byte from painting a solid block. */
	if (glyph_index > 126)
		glyph_index = ' ';

	const uint8_t *glyph = &font8x8[(size_t)glyph_index * FONT8X8_HEIGHT];
	uint32_t fgp = pack_rgb(palette_rgb[fb.fg & 0x0F]);
	uint32_t bgp = pack_rgb(palette_rgb[fb.bg & 0x0F]);

	uint32_t px0 = fb.origin_x + col * CELL_W * fb.scale;
	uint32_t py0 = fb.origin_y + row * CELL_H * fb.scale;

	for (uint32_t gy = 0; gy < CELL_H; gy++) {
		uint8_t bits = glyph[gy];
		for (uint32_t gx = 0; gx < CELL_W; gx++) {
			/* MSB-first: bit 7 is the leftmost pixel.  The
			   inherited font table is already stored in this
			   order, so there is no per-pixel reversal here. */
			uint32_t pixel = (bits & (0x80u >> gx)) ? fgp : bgp;

			for (uint32_t sy = 0; sy < fb.scale; sy++) {
				for (uint32_t sx = 0; sx < fb.scale; sx++) {
					put_pixel(px0 + gx * fb.scale + sx,
					          py0 + gy * fb.scale + sy,
					          pixel);
				}
			}
		}
	}
}

static void rgb_scroll(void)
{
	uint32_t line_h = CELL_H * fb.scale;
	uint32_t top    = fb.origin_y;
	uint32_t used_h = fb.rows * line_h;
	uint32_t bgp    = pack_rgb(palette_rgb[fb.bg & 0x0F]);

	/* Move the text area up by one line.  This is a straight memmove per
	   scanline rather than per pixel: on a 1024x768x32 panel that is
	   3 MiB a scroll, and the per-pixel version was visibly slow. */
	for (uint32_t y = top + line_h; y < top + used_h; y++) {
		volatile uint8_t *dst = fb.base + (size_t)(y - line_h) * fb.pitch;
		volatile uint8_t *src = fb.base + (size_t)y * fb.pitch;
		memcpy((void *)dst, (const void *)src,
		       (size_t)fb.width_px * fb.bytes_per_pixel);
	}

	/* Clear the freed bottom line. */
	for (uint32_t y = top + used_h - line_h; y < top + used_h; y++)
		for (uint32_t x = 0; x < fb.width_px; x++)
			put_pixel(x, y, bgp);
}

/* ── Shared grid logic ───────────────────────────────────────────────── */

static void cell_put(uint32_t col, uint32_t row, char c)
{
	if (col >= fb.cols || row >= fb.rows)
		return;
	if (fb.kind == FB_EGA_TEXT)
		ega_cell_put(col, row, c);
	else
		rgb_cell_put(col, row, c);
}

static void do_scroll(void)
{
	if (fb.kind == FB_EGA_TEXT)
		ega_scroll();
	else
		rgb_scroll();
}

static void fbcon_clear(void)
{
	if (fb.kind == FB_EGA_TEXT) {
		for (uint32_t y = 0; y < fb.rows; y++)
			for (uint32_t x = 0; x < fb.cols; x++)
				ega_cell_put(x, y, ' ');
	} else {
		uint32_t bgp = pack_rgb(palette_rgb[fb.bg & 0x0F]);
		for (uint32_t y = 0; y < fb.height_px; y++)
			for (uint32_t x = 0; x < fb.width_px; x++)
				put_pixel(x, y, bgp);
	}
	fb.cx = 0;
	fb.cy = 0;
}

static void newline(void)
{
	fb.cx = 0;
	if (++fb.cy >= fb.rows) {
		do_scroll();
		fb.cy = fb.rows - 1;
	}
}

static void fbcon_putc(char c)
{
	switch (c) {
	case '\n':
		newline();
		return;
	case '\r':
		fb.cx = 0;
		return;
	case '\t':
		/* Tabs land on 8-column stops.  Advancing by a fixed 8 (as
		   HavenDOS's shell did) misaligns every column after the
		   first tab on a line. */
		do {
			cell_put(fb.cx, fb.cy, ' ');
			if (++fb.cx >= fb.cols) {
				newline();
				return;
			}
		} while (fb.cx % 8);
		return;
	case '\b':
		if (fb.cx > 0) {
			fb.cx--;
			cell_put(fb.cx, fb.cy, ' ');
		}
		return;
	case '\0':
		return;
	default:
		break;
	}

	cell_put(fb.cx, fb.cy, c);
	if (++fb.cx >= fb.cols)
		newline();
}

static void fbcon_write(const char *buf, size_t len)
{
	if (!fb.ready)
		return;
	for (size_t i = 0; i < len; i++)
		fbcon_putc(buf[i]);
}

static struct console fbcon_console = {
	.name      = "display",
	.write     = fbcon_write,
	.min_level = LOG_INFO,
};

/* ── Bring-up ────────────────────────────────────────────────────────── */

int fbcon_init(const struct boot_framebuffer *bfb)
{
	if (!bfb || bfb->format == BOOT_FB_NONE || bfb->address == 0)
		return -1;

	memset(&fb, 0, sizeof(fb));

	/* The framebuffer is physical memory (or an MMIO aperture); reach it
	   through the direct map rather than assuming it is identity
	   mapped, which it will not be once the identity window is torn
	   down. */
	fb.base      = (volatile uint8_t *)arch_phys_to_virt(bfb->address);
	fb.pitch     = bfb->pitch;
	fb.width_px  = bfb->width;
	fb.height_px = bfb->height;

	if (bfb->format == BOOT_FB_EGA_TEXT) {
		fb.kind  = FB_EGA_TEXT;
		fb.cols  = bfb->width;
		fb.rows  = bfb->height;
		fb.scale = 1;
	} else {
		fb.kind            = FB_RGB;
		fb.bytes_per_pixel = (uint8_t)((bfb->bpp + 7) / 8);
		fb.r_shift = bfb->red_shift;   fb.r_bits = bfb->red_bits;
		fb.g_shift = bfb->green_shift; fb.g_bits = bfb->green_bits;
		fb.b_shift = bfb->blue_shift;  fb.b_bits = bfb->blue_bits;

		/* Guard against a firmware report of zero-width channels,
		   which would make pack_rgb() shift by 8 and lose the
		   channel entirely. */
		if (!fb.r_bits || !fb.g_bits || !fb.b_bits ||
		    fb.r_bits > 8 || fb.g_bits > 8 || fb.b_bits > 8) {
			fb.r_shift = 16; fb.r_bits = 8;
			fb.g_shift = 8;  fb.g_bits = 8;
			fb.b_shift = 0;  fb.b_bits = 8;
		}

		/* Pick the largest integer glyph scale that still leaves a
		   usable number of columns.  80 columns is the floor because
		   the whole kernel log is formatted for it. */
		fb.scale = 1;
		while ((bfb->width / (CELL_W * (fb.scale + 1))) >= 80 &&
		       (bfb->height / (CELL_H * (fb.scale + 1))) >= 25 &&
		       fb.scale < 4)
			fb.scale++;

		fb.cols = bfb->width  / (CELL_W * fb.scale);
		fb.rows = bfb->height / (CELL_H * fb.scale);

		/* Centre the grid in the panel, so the text block is not
		   wedged into the top-left of a large display. */
		fb.origin_x = (bfb->width  - fb.cols * CELL_W * fb.scale) / 2;
		fb.origin_y = (bfb->height - fb.rows * CELL_H * fb.scale) / 2;
	}

	if (fb.cols == 0 || fb.rows == 0)
		return -1;
	if (fb.cols > MAX_COLS)
		fb.cols = MAX_COLS;
	if (fb.rows > MAX_ROWS)
		fb.rows = MAX_ROWS;

	fb.fg    = COLOR_FG;
	fb.bg    = COLOR_BG;
	fb.ready = true;

	fbcon_clear();
	console_register(&fbcon_console);

	kinfo("fbcon: %s %ux%u, %ux%u cells, glyph scale %u",
	      fb.kind == FB_EGA_TEXT ? "EGA text" : "linear RGB",
	      fb.width_px, fb.height_px, fb.cols, fb.rows, fb.scale);

	return 0;
}

void fbcon_geometry(uint32_t *cols, uint32_t *rows)
{
	if (cols)
		*cols = fb.ready ? fb.cols : 0;
	if (rows)
		*rows = fb.ready ? fb.rows : 0;
}

bool fbcon_available(void)
{
	return fb.ready;
}
