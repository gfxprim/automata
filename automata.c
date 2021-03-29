// SPDX-License-Identifier: GPL-2.1-or-later
/*
 * Copyright (C) 2021 Richard Palethorpe (richiejp.com)
 */

#include <stdlib.h>
#include <errno.h>
#include <gfxprim.h>

/* If bit n is 1 then make all bits 1 otherwise 0 */
#define BIT_TO_MAX(b, n) (((b >> n) & 1) * ~0UL)

/* Number of bitfields in a row */
static size_t width = 1;
/* Number of steps in the simulation */
static size_t height = 64;
/* Matrix of bitfields representing the automata's state over time */
static uint64_t *steps;
/* Initial conditions */
static uint64_t *init;
/* Zero row mask */
static uint64_t *zeroes;
/* Numeric representation of the current update rule */
static uint8_t rule = 110;
/* Whether to use the reversible version of the current rule */
static int reversible;

static void *uids;

static void ca1d_allocate(void)
{
	if (init)
		gp_vec_free(init);
	init = gp_vec_new(width, sizeof(uint64_t));
	init[width / 2] = 1UL << (63 - (width * 32) % 64);

	if (zeroes)
		gp_vec_free(zeroes);
	zeroes = gp_vec_new(width, sizeof(uint64_t));

	if (steps)
		gp_vec_free(steps);
	steps = gp_matrix_new(width, height, sizeof(uint64_t));
}

/* Apply the current rule to a 64bit segment of a row */
static inline uint64_t ca1d_rule_apply(uint64_t c_prev,
				       uint64_t c,
				       uint64_t c_next,
				       uint64_t c_prev_step)
{
	int i;
	uint64_t l = (c >> 1) ^ (c_prev << 63);
	uint64_t r = (c << 1) ^ (c_next >> 63);
	uint64_t c_next_step = 0;

	for (i = 0; i < 8; i++) {
		uint64_t active = BIT_TO_MAX(rule, i);
		uint64_t left   = BIT_TO_MAX(i, 2);
		uint64_t center = BIT_TO_MAX(i, 1);
		uint64_t right  = BIT_TO_MAX(i, 0);

		c_next_step |=
			active & ~(left ^ l) & ~(center ^ c) & ~(right ^ r);
	}

	return c_next_step ^ c_prev_step;
}

/* Apply the current rule to an entire row */
static inline void ca1d_rule_apply_row(const uint64_t *prev,
				       const uint64_t *cur,
				       uint64_t *next)
{
	size_t i;

	next[0] = ca1d_rule_apply(cur[width - 1], cur[0],
				  cur[GP_MIN(1, width - 1)], prev[0]);

	for (i = 1; i < width - 1; i++) {
		next[i] = ca1d_rule_apply(cur[i - 1], cur[i], cur[i + 1],
					  prev[i]);
	}

	if (i >= width)
		return;

	next[i] = ca1d_rule_apply(cur[i - 1], cur[i], cur[0], prev[i]);
}

static void ca1d_run(void)
{
	const uint64_t *prev = zeroes;
	const uint64_t *cur = steps;
	uint64_t *next = steps + gp_matrix_idx(width, 1, 0);
	size_t i = 1;

	memcpy(steps, init, width * sizeof(uint64_t));

	for (;;) {
		ca1d_rule_apply_row(prev, cur, next);

		if (++i >= height)
			break;

		prev = reversible ? cur : zeroes;
		cur = next;
		next = steps + gp_matrix_idx(width, i, 0);
	}

}

/* Note that i & 63 = i % 64 and i >> 6 = i / 64 as 2**6 = 64. Also
 * use putpixel_raw because it is inlined and we know x and y are
 * inside the pixmap.
 */
static inline void shade_pixel(gp_pixmap *p,
			       float pw, float ph,
			       uint32_t x, uint32_t y,
			       gp_pixel bg, gp_pixel fg)
{
	size_t i = (float)x * pw;
	size_t j = (float)y * ph;
	size_t k = 63 - (i & 63);
	uint64_t c = steps[gp_matrix_idx(width, j, i >> 6)];

	c = BIT_TO_MAX(c, k);
	gp_putpixel_raw(p, x, y, (fg & c) | (bg & ~c));
}

static void fill_pixmap(gp_pixmap *p)
{
	uint32_t x, y;
	gp_pixel bg = gp_rgb_to_pixmap_pixel(0xff, 0xff, 0xff, p);
	gp_pixel fg = gp_rgb_to_pixmap_pixel(0x00, 0x00, 0x00, p);
	gp_pixel fill = gp_rgb_to_pixmap_pixel(0xff, 0x00, 0x00, p);
	uint64_t s, t;

	s = gp_time_stamp();
	gp_fill(p, fill);
	t = gp_time_stamp();

	printf("Fill time %lums\n", t - s);

	s = gp_time_stamp();
	ca1d_run();
	t = gp_time_stamp();

	printf("Automata time %lums\n", t - s);

	if (width * 64 > p->w || height > p->h) {
		printf("Automata is larger than screen\n");
		return;
	}

	float pw = (float)(64 * width) / (float)p->w;
	float ph = (float)height / (float)p->h;

	s = gp_time_stamp();
	for (y = 0; y < p->h; y++) {
		for (x = 0; x < p->w; x++)
			shade_pixel(p, pw, ph, x, y, bg, fg);
	}
	t = gp_time_stamp();

	printf("Fill rects time %lums\n", t - s);
}

static void allocate_backing_pixmap(gp_widget_event *ev)
{
	gp_widget *w = ev->self;
	gp_size l = w->w & 63 ? w->w + 64 - (w->w & 63) : w->w;
	gp_size h = w->h;

	gp_pixmap_free(w->pixmap->pixmap);

	w->pixmap->pixmap = gp_pixmap_alloc(l, h, ev->ctx->pixel_type);

	fill_pixmap(w->pixmap->pixmap);
}

int pixmap_on_event(gp_widget_event *ev)
{
	gp_widget_event_dump(ev);

	switch (ev->type) {
	case GP_WIDGET_EVENT_RESIZE:
		allocate_backing_pixmap(ev);
	break;
	default:
	break;
	}

	return 0;
}

static void pixmap_do_redraw(void)
{
	gp_widget *pixmap = gp_widget_by_uid(uids, "pixmap", GP_WIDGET_PIXMAP);

	fill_pixmap(pixmap->pixmap->pixmap);
	gp_widget_redraw(pixmap);
}

int rule_widget_on_event(gp_widget_event *ev)
{
	struct gp_widget_tbox *tb = ev->self->tbox;
	char tbuf[4] = { 0 };
	char c;
	int r;

	if (ev->type != GP_WIDGET_EVENT_WIDGET)
		return 0;

	switch(ev->self->type) {
	case GP_WIDGET_TBOX:
		switch(ev->sub_type) {
		case GP_WIDGET_TBOX_FILTER:
			c = (char)ev->val;

			if (c < '0' || c > '9')
				return 1;

			if (!gp_vec_strlen(tb->buf))
				return 0;

			strcpy(tbuf, tb->buf);
			tbuf[tb->cur_pos] = c;

			r = strtol(tbuf, NULL, 10);

			return r > 255;
			break;
		case GP_WIDGET_TBOX_EDIT:
			rule = strtol(tb->buf, NULL, 10);
			break;
		default:
			break;
		}
		break;
	case GP_WIDGET_CHECKBOX:
		reversible = ev->self->checkbox->val;
		break;
	default:
		return 0;
	}

	pixmap_do_redraw();

	return 0;
}

static void init_from_text(void)
{
	gp_widget *self = gp_widget_by_uid(uids, "init", GP_WIDGET_TBOX);
	const char *text = gp_widget_tbox_text(self);
	size_t len = gp_vec_strlen(text);

	memset(init, 0, width * sizeof(uint64_t));

	if (!len)
		init[width / 2] = 1UL << (63 - (width * 32) % 64);
	else
		memcpy(init, text, GP_MIN(width * sizeof(uint64_t), len));
}

int width_widget_on_event(gp_widget_event *ev)
{
	struct gp_widget_tbox *tb = ev->self->tbox;
	char c;

	if (ev->type != GP_WIDGET_EVENT_WIDGET)
		return 0;

	switch(ev->sub_type) {
	case GP_WIDGET_TBOX_FILTER:
		c = (char)ev->val;

		return c < '0' || c > '9';

		break;
	case GP_WIDGET_TBOX_EDIT:
		if (!gp_vec_strlen(tb->buf))
			return 0;

		width = GP_MAX(1, strtol(tb->buf, NULL, 10));
		ca1d_allocate();
		init_from_text();
		pixmap_do_redraw();
		break;
	default:
		break;
	}

	return 0;
}

int height_widget_on_event(gp_widget_event *ev)
{
	struct gp_widget_tbox *tb = ev->self->tbox;
	char c;

	if (ev->type != GP_WIDGET_EVENT_WIDGET)
		return 0;

	switch(ev->sub_type) {
	case GP_WIDGET_TBOX_FILTER:
		c = (char)ev->val;

		return c < '0' || c > '9';

		break;
	case GP_WIDGET_TBOX_EDIT:
		if (!gp_vec_strlen(tb->buf))
			return 0;

		height = GP_MAX(2, strtol(tb->buf, NULL, 10));
		ca1d_allocate();
		init_from_text();
		pixmap_do_redraw();
		break;
	default:
		break;
	}

	return 0;
}

int init_widget_on_event(gp_widget_event *ev)
{

	if (ev->type != GP_WIDGET_EVENT_WIDGET)
		return 0;

	switch(ev->sub_type) {
	case GP_WIDGET_TBOX_EDIT:
		init_from_text();
		pixmap_do_redraw();
		break;
	default:
		break;
	}

	return 0;
}

int select_dir_on_event(gp_widget_event *ev)
{
	const char *path;
	gp_widget_dialog *dialog;
	gp_widget *path_tbox;

	if (ev->type != GP_WIDGET_EVENT_WIDGET)
		return 0;

	dialog = gp_widget_dialog_file_open_new(NULL);

	if (gp_widget_dialog_run(dialog) != GP_WIDGET_DIALOG_PATH)
		goto out;

	path = gp_widget_dialog_file_open_path(dialog);
	printf("Selected path '%s'\n", path);

	path_tbox = gp_widget_by_uid(uids, "file path", GP_WIDGET_TBOX);
	gp_widget_tbox_printf(path_tbox, "%s1dca.jpeg", path);

out:
	gp_widget_dialog_free(dialog);

	return 0;
}

int save_on_event(gp_widget_event *ev)
{
	const char *path;
	gp_widget *pixmap_w;
	gp_pixmap *pixmap;
	gp_widget *path_tbox;

	if (ev->type != GP_WIDGET_EVENT_WIDGET)
		return 0;

	path_tbox = gp_widget_by_uid(uids, "file path", GP_WIDGET_TBOX);
	path = gp_widget_tbox_text(path_tbox);

	pixmap_w = gp_widget_by_uid(uids, "pixmap", GP_WIDGET_PIXMAP);
	pixmap = pixmap_w->pixmap->pixmap;
	if (gp_save_image(pixmap, path, NULL)) {
		switch(errno) {
		case EINVAL:
			perror("File extension not found or pixel type not supported by format");
			break;
		case ENOSYS:
			perror("Image format not supported");
			break;
		default:
			perror("Save image failed");
		}
	}

	return 0;
}

int main(int argc, char *argv[])
{
	gp_widget *layout = gp_app_layout_load("automata", &uids);

	if (!layout)
		return 0;

	gp_widget *pixmap = gp_widget_by_uid(uids, "pixmap", GP_WIDGET_PIXMAP);

	gp_widget_event_unmask(pixmap, GP_WIDGET_EVENT_RESIZE);

	ca1d_allocate();
	gp_widgets_main_loop(layout, "Pixmap example", NULL, argc, argv);

	return 0;
}
