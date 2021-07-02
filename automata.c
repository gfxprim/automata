// SPDX-License-Identifier: GPL-2.1-or-later
/*
 * Copyright (C) 2021 Richard Palethorpe (richiejp.com)
 */

#include <stdlib.h>
#include <errno.h>
#include <unistd.h>
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
/* The number of rules to alternate between */
static uint8_t rule_n = 1;
/* Numeric representation of the current update rules */
static uint8_t rules[256] = { 110 };
/* Whether to use the reversible version of the current rule */
static int reversible;
/* Meta update rule which changes the rule being used */
static uint8_t meta_rule = 0;

static gp_htable *uids;

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

/* Count the number of set bits using classic "Magic Numbers" algorithm.
 *
 * Binary Magic Numbers, by Edwin E. Freed; Dr. Dobbs Journal, April 1983
 * https://archive.org/details/1983-04-dr-dobbs-journal/page/24/mode/1up
 */
__attribute__((const))
static inline uint8_t pop_count(const uint64_t bit_field)
{
	const uint64_t B[6] = {
		0x5555555555555555,
		0x3333333333333333,
		0x0f0f0f0f0f0f0f0f,
		0x00ff00ff00ff00ff,
		0x0000ffff0000ffff,
		0x00000000ffffffff
	};
	int i;
	uint64_t ret = bit_field;

	for (i = 0; i < 6; i++)
		ret = ((ret >> (1 << i)) & B[i]) + (ret & B[i]);

	return ret;
}

/* Apply the current rule to a 64bit segment of a row */
__attribute__((const))
static inline uint64_t ca1d_rule_apply(const uint8_t rule,
				       const uint64_t c_prev,
				       const uint64_t c,
				       const uint64_t c_next,
				       const uint64_t c_prev_step)
{
	int i;
	const uint64_t l = (c >> 1) ^ (c_prev << 63);
	const uint64_t r = (c << 1) ^ (c_next >> 63);
	uint64_t c_next_step = 0;

	for (i = 0; i < 8; i++) {
		const uint64_t active = BIT_TO_MAX(rule, i);
		const uint64_t left   = BIT_TO_MAX(i, 2);
		const uint64_t center = BIT_TO_MAX(i, 1);
		const uint64_t right  = BIT_TO_MAX(i, 0);

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

	next[0] = ca1d_rule_apply(rules[0],
				  cur[width - 1],
				  cur[0],
				  cur[GP_MIN(1, width - 1)],
				  prev[0]);

	for (i = 1; i < width - 1; i++) {
		next[i] = ca1d_rule_apply(rules[i % rule_n],
					  cur[i - 1],
					  cur[i],
					  cur[i + 1],
					  prev[i]);
	}

	if (i >= width)
		return;

	next[i] = ca1d_rule_apply(rules[i % rule_n],
				  cur[i - 1],
				  cur[i],
				  cur[0],
				  prev[i]);
}

static inline uint8_t ca1d_meta_rule_apply(const uint8_t rule,
					   const uint64_t c_prev,
					   const uint64_t c,
					   const uint64_t c_next)
{
	int i;
	const int pl = pop_count(c_prev) > 32 ? 1 : 0;
	const int pc = pop_count(c) > 32 ? 1 : 0;
	const int pr = pop_count(c_next) > 32 ? 1 : 0;
	int c_next_step = 0;

	for (i = 0; i < 8; i++) {
		const int active = (rule >> i) & 1;
		const int left   = (i >> 2) & 1;
		const int center = (i >> 1) & 1;
		const int right  = i & 1;

		c_next_step |=
			active & ~(left ^ pl) & ~(center ^ pc) & ~(right ^ pr);
	}

	return rules[c_next_step];
}

static inline void ca1d_meta_rule_apply_row(const uint64_t *prev,
					    const uint64_t *cur,
					    uint64_t *next)
{
	size_t i;
	uint64_t c_prev = cur[width - 1],
		c = cur[0],
		c_next = cur[GP_MIN(1, width - 1)];
	uint8_t rule = ca1d_meta_rule_apply(meta_rule, c_prev, c, c_next);

	next[0] = ca1d_rule_apply(rule, c_prev, c, c_next, prev[0]);

	for (i = 1; i < width - 1; i++) {
		c_prev = cur[i - 1];
		c = cur[i];
		c_next = cur[i + 1];

		rule = ca1d_meta_rule_apply(meta_rule, c_prev, c, c_next);
		next[i] = ca1d_rule_apply(rule, c_prev, c, c_next, prev[i]);
	}

	if (i >= width)
		return;

	c_prev = cur[i - 1];
	c = cur[i];
	c_next = cur[0];

	rule = ca1d_meta_rule_apply(meta_rule, c_prev, c, c_next);
	next[i] = ca1d_rule_apply(rule, c_prev, c, c_next, prev[i]);
}


static void ca1d_run(void)
{
	const uint64_t *prev = zeroes;
	const uint64_t *cur = steps;
	uint64_t *next = steps + gp_matrix_idx(width, 1, 0);
	size_t i = 1;

	memcpy(steps, init, width * sizeof(uint64_t));

	for (;;) {
		if (meta_rule)
			ca1d_meta_rule_apply_row(prev, cur, next);
		else
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

static void parse_rule_nums(const char *const rules_str)
{
	const char *c = rules_str;
	uint8_t rule_acc = 0;
	uint8_t rule_indx = 0;

	while (*c) {
		switch (*c) {
		case '0' ... '9':
			if (rule_acc > 25)
				goto out;

			rule_acc *= 10;
			rule_acc += ((*c) - '0');
			break;
		case ',':
		case ';':
			rules[rule_indx] = rule_acc;
			rule_indx++;
			rule_acc = 0;
			break;
		case ' ':
			break;
		default:
			return;
		}

		c++;
	}

out:
	rules[rule_indx] = rule_acc;
	rule_n = rule_indx + 1;
}

int rule_widget_on_event(gp_widget_event *ev)
{
	struct gp_widget_tbox *tb = ev->self->tbox;

	if (ev->type != GP_WIDGET_EVENT_WIDGET)
		return 0;

	switch(ev->self->type) {
	case GP_WIDGET_TBOX:
		switch(ev->sub_type) {
		case GP_WIDGET_TBOX_FILTER:
			switch ((char)ev->val) {
			case '0' ... '9':
			case ',':
			case ';':
				return 0;
			}

			return 1;
		case GP_WIDGET_TBOX_EDIT:
			parse_rule_nums(tb->buf);
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

int meta_rule_widget_on_event(gp_widget_event *ev)
{
	struct gp_widget_tbox *tb = ev->self->tbox;

	if (ev->type != GP_WIDGET_EVENT_WIDGET)
		return 0;

	switch(ev->self->type) {
	case GP_WIDGET_TBOX:
		switch(ev->sub_type) {
		case GP_WIDGET_TBOX_FILTER:
			switch ((char)ev->val) {
			case '0' ... '9':
				return 0;
			}

			return 1;
		case GP_WIDGET_TBOX_EDIT:
			meta_rule = (uint8_t)strtoul(tb->buf, NULL, 10);
			break;
		default:
			break;
		}
		break;
	default:
		return 0;
	}

	pixmap_do_redraw();

	return 0;
}

static void init_from_str(const char *text, size_t len)
{
	memset(init, 0, width * sizeof(uint64_t));

	if (!len)
		init[width / 2] = 1UL << (63 - (width * 32) % 64);
	else
		memcpy(init, text, GP_MIN(width * sizeof(uint64_t), len));
}

static void init_from_text(void)
{
	gp_widget *self = gp_widget_by_uid(uids, "init", GP_WIDGET_TBOX);
	const char *text = gp_widget_tbox_text(self);
	size_t len = gp_vec_strlen(text);

	init_from_str(text, len);
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
	gp_dialog *dialog;
	gp_widget *path_tbox;

	if (ev->type != GP_WIDGET_EVENT_WIDGET)
		return 0;

	dialog = gp_dialog_file_open_new(NULL);

	if (gp_dialog_run(dialog) != GP_WIDGET_DIALOG_PATH)
		goto out;

	path = gp_dialog_file_open_path(dialog);
	printf("Selected path '%s'\n", path);

	path_tbox = gp_widget_by_uid(uids, "file path", GP_WIDGET_TBOX);
	gp_widget_tbox_printf(path_tbox, "%s1dca.jpeg", path);

out:
	gp_dialog_free(dialog);

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
	if (gp_save_image(pixmap, path, NULL))
		gp_dialog_msg_printf_run(GP_DIALOG_MSG_ERR, "Save Failed", "%s", strerror(errno));

	return 0;
}

int widgets_main(int argc, char *argv[])
{
	gp_widget *layout = gp_app_layout_load("automata", &uids);

	if (!layout)
		return 0;

	gp_widget *pixmap = gp_widget_by_uid(uids, "pixmap", GP_WIDGET_PIXMAP);

	gp_widget_event_unmask(pixmap, GP_WIDGET_EVENT_RESIZE);
	gp_widgets_main_loop(layout, "Automata", NULL, argc, argv);
}

int main(int argc, char *argv[])
{
	int c;
	const char *init_arg = NULL;
	const char *save_path = NULL;
	float scale = 1;

	while ((c = getopt(argc, argv, "+w:h:i:m:f:r:es:")) != -1) {
		switch(c) {
		case 'w':
			width = strtoul(optarg, NULL, 10);
			break;
		case 'h':
			height = strtoul(optarg, NULL, 10);
			break;
		case 'i':
			init_arg = optarg;
			break;
		case 'm':
			meta_rule = strtoul(optarg, NULL, 10);
			break;
		case 'f':
			save_path = optarg;
			break;
		case 'r':
			parse_rule_nums(optarg);
			break;
		case 'e':
			reversible = 1;
			break;
		case 's':
			scale = strtof(optarg, NULL);
			break;
		default:
			fprintf(stderr,
				"Usage:\n\t%s [-w <width>][-h <height>][-i <initial conditions>][-f <save file>][-r <rule>][-m <meta_rule>][-e][-s <scale>]\n",
				argv[0]);
			return 1;
		}
	}

	ca1d_allocate();

	if (init_arg)
		init_from_str(init_arg, strlen(init_arg));

	gp_set_debug_level(3);

	if (!save_path)
		return widgets_main(argc, argv);

	gp_pixmap *pxm = gp_pixmap_alloc(width * 64 * scale, height * scale, GP_PIXEL_G1);
	gp_pixel bg = gp_rgb_to_pixmap_pixel(0xff, 0xff, 0xff, pxm);
	gp_pixel fg = gp_rgb_to_pixmap_pixel(0x00, 0x00, 0x00, pxm);

	ca1d_run();

	for (uint32_t y = 0; y < height * scale; y++) {
		for (uint32_t x = 0; x < width * 64 * scale; x++)
			shade_pixel(pxm, 1.0f / scale, 1.0f / scale, x, y, bg, fg);
	}

	if (gp_save_image(pxm, save_path, NULL)) {
		perror("Save Failed!");
		return 1;
	}

	return 0;
}
