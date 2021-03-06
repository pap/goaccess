/**
 * gdashboard.c -- goaccess main dashboard
 * Copyright (C) 2009-2014 by Gerardo Orellana <goaccess@prosoftcorp.com>
 * GoAccess - An Ncurses apache weblog analyzer & interactive viewer
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of
 * the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * A copy of the GNU General Public License is attached to this
 * source distribution for its full text.
 *
 * Visit http://goaccess.prosoftcorp.com for new releases.
 */

#define _XOPEN_SOURCE 700

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <regex.h>

#include "gdashboard.h"

#ifdef HAVE_LIBTOKYOCABINET
#include "tcabdb.h"
#else
#include "glibht.h"
#endif

#include "error.h"
#include "color.h"
#include "util.h"
#include "xmalloc.h"

static GFind find_t;

/* reset find indices */
void
reset_find (void)
{
  if (find_t.pattern != NULL && *find_t.pattern != '\0')
    free (find_t.pattern);

  find_t.look_in_sub = 0;
  find_t.module = 0;
  find_t.next_idx = 0;  /* next total index    */
  find_t.next_parent_idx = 0;   /* next parent index   */
  find_t.next_sub_idx = 0;      /* next sub item index */
  find_t.pattern = NULL;
}

/* allocate memory for dash */
GDash *
new_gdash (void)
{
  GDash *dash = xmalloc (sizeof (GDash));
  memset (dash, 0, sizeof *dash);
  dash->total_alloc = 0;

  return dash;
}

/* allocate memory for dash elements */
GDashData *
new_gdata (uint32_t size)
{
  GDashData *data = xcalloc (size, sizeof (GDashData));

  return data;
}

static void
free_dashboard_data (GDashData item)
{
  if (item.metrics == NULL)
    return;

  if (item.metrics->data)
    free (item.metrics->data);
  if (item.metrics->bw.sbw)
    free (item.metrics->bw.sbw);
  if (conf.serve_usecs && item.metrics->avgts.sts)
    free (item.metrics->avgts.sts);
  if (conf.serve_usecs && item.metrics->cumts.sts)
    free (item.metrics->cumts.sts);
  if (conf.serve_usecs && item.metrics->maxts.sts)
    free (item.metrics->maxts.sts);
  free (item.metrics);
}

/* free dash and its elements */
void
free_dashboard (GDash * dash)
{
  int i, j;
  for (i = 0; i < TOTAL_MODULES; i++) {
    for (j = 0; j < dash->module[i].alloc_data; j++) {
      free_dashboard_data (dash->module[i].data[j]);
    }
    free (dash->module[i].data);
  }
  free (dash);
}

static GModule
get_find_current_module (GDash * dash, int offset)
{
  GModule module;

  for (module = 0; module < TOTAL_MODULES; module++) {
    /* set current module */
    if (dash->module[module].pos_y == offset)
      return module;
    /* we went over by one module, set current - 1 */
    if (dash->module[module].pos_y > offset)
      return module - 1;
  }

  return 0;
}

/* Get the number of rows that a collapsed dashboard panel contains */
int
get_num_collapsed_data_rows (void)
{
  int size = DASH_COLLAPSED - DASH_NON_DATA;
  return conf.no_column_names ? size + DASH_COL_ROWS : size;
}

/* Get the number of rows that the expanded dashboard panel contains */
int
get_num_expanded_data_rows (void)
{
  int size = DASH_EXPANDED - DASH_NON_DATA;
  return conf.no_column_names ? size + DASH_COL_ROWS : size;
}

/* Get the Y position where data rows start */
static int
get_data_pos_rows (void)
{
  return conf.no_column_names ? DASH_DATA_POS - DASH_COL_ROWS : DASH_DATA_POS;
}

static int
get_xpos (void)
{
  return DASH_INIT_X;
}

/* Determine which module should be expanded given the
 * current mouse position. */
int
set_module_from_mouse_event (GScroll * gscroll, GDash * dash, int y)
{
  int module = 0;
  int offset = y - MAX_HEIGHT_HEADER - MAX_HEIGHT_FOOTER + 1;
  if (gscroll->expanded) {
    module = get_find_current_module (dash, offset);
  } else {
    offset += gscroll->dash;
    module = offset / DASH_COLLAPSED;
  }

  if (module >= TOTAL_MODULES)
    module = TOTAL_MODULES - 1;
  else if (module < 0)
    module = 0;

  if ((int) gscroll->current == module)
    return 1;

  gscroll->current = module;
  return 0;
}

/* render child nodes */
static char *
render_child_node (const char *data)
{
  char *buf;
  int len = 0;

#ifdef HAVE_LIBNCURSESW
  const char *bend = "\xe2\x94\x9c";
  const char *horz = "\xe2\x94\x80";
#else
  const char *bend = "|";
  const char *horz = "`-";
#endif

  if (data == NULL || *data == '\0')
    return NULL;

  len = snprintf (NULL, 0, " %s%s %s", bend, horz, data);
  buf = xmalloc (len + 3);
  sprintf (buf, " %s%s %s", bend, horz, data);

  return buf;
}

/* get a string of bars given current hits, maximum hit & xpos */
static char *
get_bars (int n, int max, int x)
{
  int w, h, len;

  getmaxyx (stdscr, h, w);
  (void) h;     /* avoid lint warning */

  if ((len = (n * (w - x) / max)) < 1)
    len = 1;
  return char_repeat (len, '|');
}

/*get largest method's length */
static int
get_max_method_len (GDashData * data, int size)
{
  int i, max = 0, len;
  for (i = 0; i < size; i++) {
    if (data[i].metrics->method == NULL)
      continue;
    len = strlen (data[i].metrics->method);
    if (len > max)
      max = len;
  }
  return max;
}

/*get largest data's length */
static int
get_max_data_len (GDashData * data, int size)
{
  int i, max = 0, len;
  for (i = 0; i < size; i++) {
    if (data[i].metrics->data == NULL)
      continue;
    len = strlen (data[i].metrics->data);
    if (len > max)
      max = len;
  }
  return max;
}

/*get largest hit's length */
static int
get_max_visitor_len (GDashData * data, int size)
{
  int i, max = 0;
  for (i = 0; i < size; i++) {
    int len = intlen (data[i].metrics->visitors);
    if (len > max)
      max = len;
  }

  if (!conf.no_column_names && max < COLUMN_VIS_LEN)
    max = COLUMN_VIS_LEN;

  return max;
}

/*get largest hit's length */
static int
get_max_hit_len (GDashData * data, int size)
{
  int i, max = 0;
  for (i = 0; i < size; i++) {
    int len = intlen (data[i].metrics->hits);
    if (len > max)
      max = len;
  }

  if (!conf.no_column_names && max < COLUMN_HITS_LEN)
    max = COLUMN_HITS_LEN;

  return max;
}

/* get largest hit */
static int
get_max_hit (GDashData * data, int size)
{
  int i, max = 0;
  for (i = 0; i < size; i++) {
    int cur = 0;
    if ((cur = data[i].metrics->hits) > max)
      max = cur;
  }
  return max;
}

static int
get_max_perc_len (int max_percent)
{
  return intlen ((int) max_percent);
}

/* set item's percent in GDashData */
static float
set_percent_data (GDashData * data, int n, int valid)
{
  float max = 0.0;
  int i;
  for (i = 0; i < n; i++) {
    data[i].metrics->percent = get_percentage (valid, data[i].metrics->hits);
    if (data[i].metrics->percent > max)
      max = data[i].metrics->percent;
  }
  return max;
}

/* render module's total */
static void
render_total_label (WINDOW * win, GDashModule * data, int y,
                    GColors * (*func) (void))
{
  char *s;
  int win_h, win_w, total, ht_size;

  total = data->holder_size;
  ht_size = data->ht_size;

  s = xmalloc (snprintf (NULL, 0, "Total: %d/%d", total, ht_size) + 1);
  getmaxyx (win, win_h, win_w);
  (void) win_h;

  sprintf (s, "Total: %d/%d", total, ht_size);
  draw_header (win, s, "%s", y, win_w - strlen (s) - 2, win_w, func);
  free (s);
}

/* render dashboard bars (graph) */
static void
render_bars (GDashModule * data, GDashRender render, int *x)
{
  GColors *color = get_color (COLOR_BARS);
  WINDOW *win = render.win;
  char *bar;
  int y = render.y, w = render.w, idx = render.idx, sel = render.sel;

  bar = get_bars (data->data[idx].metrics->hits, data->max_hits, *x);
  if (sel)
    draw_header (win, bar, "%s", y, *x, w, color_selected);
  else {
    wattron (win, color->attr | COLOR_PAIR (color->pair->idx));
    mvwprintw (win, y, *x, "%s", bar);
    wattroff (win, color->attr | COLOR_PAIR (color->pair->idx));
  }
  free (bar);
}

static void
render_data_hosts (WINDOW * win, GDashRender render, char *value, int x)
{
  char *padded_data;

  padded_data = left_pad_str (value, x);
  draw_header (win, padded_data, "%s", render.y, 0, render.w, color_selected);
  free (padded_data);
}

static void
get_visitors_date (char *buf, const char *value)
{
  /* verify we have a valid date conversion */
  if (convert_date (buf, (char *) value, "%Y%m%d", "%d/%b/%Y", DATE_LEN) != 0) {
    LOG_DEBUG (("invalid date: %s", value));
    xstrncpy (buf, "---", 4);
  }
}

/* render dashboard data */
static void
render_data (GDashModule * data, GDashRender render, int *x)
{
  GColors *color = get_color_by_item_module (COLOR_MTRC_DATA, data->module);
  WINDOW *win = render.win;

  char buf[DATE_LEN];
  char *value;
  int y = render.y, w = render.w, idx = render.idx, sel = render.sel;

  value = substring (data->data[idx].metrics->data, 0, w - *x);
  if (data->module == VISITORS)
    get_visitors_date (buf, value);

  if (sel && data->module == HOSTS && data->data[idx].is_subitem) {
    render_data_hosts (win, render, value, *x);
  } else if (sel) {
    draw_header (win, data->module == VISITORS ? buf : value, "%s", y, *x, w,
                 color_selected);
  } else {
    wattron (win, color->attr | COLOR_PAIR (color->pair->idx));
    mvwprintw (win, y, *x, "%s", data->module == VISITORS ? buf : value);
    wattroff (win, color->attr | COLOR_PAIR (color->pair->idx));
  }

  *x += data->module == VISITORS ? DATE_LEN - 1 : data->data_len;
  *x += DASH_SPACE;
  free (value);
}

/* render dashboard request method */
static void
render_method (GDashModule * data, GDashRender render, int *x)
{
  GColors *color = get_color_by_item_module (COLOR_MTRC_MTHD, data->module);
  WINDOW *win = render.win;

  int y = render.y, w = render.w, idx = render.idx, sel = render.sel;
  char *method = data->data[idx].metrics->method;

  if (method == NULL || *method == '\0')
    return;

  if (sel) {
    /* selected state */
    draw_header (win, method, "%s", y, *x, w, color_selected);
  } else {
    /* regular state */
    wattron (win, color->attr | COLOR_PAIR (color->pair->idx));
    mvwprintw (win, y, *x, "%s", method);
    wattroff (win, color->attr | COLOR_PAIR (color->pair->idx));
  }

  *x += data->method_len + DASH_SPACE;
}

/* render dashboard request protocol */
static void
render_proto (GDashModule * data, GDashRender render, int *x)
{
  GColors *color = get_color_by_item_module (COLOR_MTRC_PROT, data->module);
  WINDOW *win = render.win;

  int y = render.y, w = render.w, idx = render.idx, sel = render.sel;
  char *protocol = data->data[idx].metrics->protocol;

  if (protocol == NULL || *protocol == '\0')
    return;

  if (sel) {
    /* selected state */
    draw_header (win, protocol, "%s", y, *x, w, color_selected);
  } else {
    /* regular state */
    wattron (win, color->attr | COLOR_PAIR (color->pair->idx));
    mvwprintw (win, y, *x, "%s", protocol);
    wattroff (win, color->attr | COLOR_PAIR (color->pair->idx));
  }

  *x += REQ_PROTO_LEN - 1 + DASH_SPACE;
}

/* render dashboard averages time served */
static void
render_avgts (GDashModule * data, GDashRender render, int *x)
{
  GColors *color = get_color_by_item_module (COLOR_MTRC_AVGTS, data->module);
  WINDOW *win = render.win;

  int y = render.y, w = render.w, idx = render.idx, sel = render.sel;
  char *avgts = data->data[idx].metrics->avgts.sts;

  if (data->module == HOSTS && data->data[idx].is_subitem)
    goto out;

  if (sel) {
    /* selected state */
    draw_header (win, avgts, "%9s", y, *x, w, color_selected);
  } else {
    /* regular state */
    wattron (win, color->attr | COLOR_PAIR (color->pair->idx));
    mvwprintw (win, y, *x, "%9s", avgts);
    wattroff (win, color->attr | COLOR_PAIR (color->pair->idx));
  }

out:
  *x += DASH_SRV_TM_LEN + DASH_SPACE;
}

/* render dashboard averages time served */
static void
render_cumts (GDashModule * data, GDashRender render, int *x)
{
  GColors *color = get_color_by_item_module (COLOR_MTRC_CUMTS, data->module);
  WINDOW *win = render.win;

  int y = render.y, w = render.w, idx = render.idx, sel = render.sel;
  char *cumts = data->data[idx].metrics->cumts.sts;

  if (data->module == HOSTS && data->data[idx].is_subitem)
    goto out;

  if (sel) {
    /* selected state */
    draw_header (win, cumts, "%9s", y, *x, w, color_selected);
  } else {
    /* regular state */
    wattron (win, color->attr | COLOR_PAIR (color->pair->idx));
    mvwprintw (win, y, *x, "%9s", cumts);
    wattroff (win, color->attr | COLOR_PAIR (color->pair->idx));
  }

out:
  *x += DASH_SRV_TM_LEN + DASH_SPACE;
}

/* render dashboard averages time served */
static void
render_maxts (GDashModule * data, GDashRender render, int *x)
{
  GColors *color = get_color_by_item_module (COLOR_MTRC_MAXTS, data->module);
  WINDOW *win = render.win;

  int y = render.y, w = render.w, idx = render.idx, sel = render.sel;
  char *maxts = data->data[idx].metrics->maxts.sts;

  if (data->module == HOSTS && data->data[idx].is_subitem)
    goto out;

  if (sel) {
    /* selected state */
    draw_header (win, maxts, "%9s", y, *x, w, color_selected);
  } else {
    /* regular state */
    wattron (win, color->attr | COLOR_PAIR (color->pair->idx));
    mvwprintw (win, y, *x, "%9s", maxts);
    wattroff (win, color->attr | COLOR_PAIR (color->pair->idx));
  }

out:
  *x += DASH_SRV_TM_LEN + DASH_SPACE;
}

/* render dashboard bandwidth */
static void
render_bw (GDashModule * data, GDashRender render, int *x)
{
  GColors *color = get_color_by_item_module (COLOR_MTRC_BW, data->module);
  WINDOW *win = render.win;

  int y = render.y, w = render.w, idx = render.idx, sel = render.sel;
  char *bw = data->data[idx].metrics->bw.sbw;

  if (data->module == HOSTS && data->data[idx].is_subitem)
    goto out;

  if (sel) {
    /* selected state */
    draw_header (win, bw, "%11s", y, *x, w, color_selected);
  } else {
    /* regular state */
    wattron (win, color->attr | COLOR_PAIR (color->pair->idx));
    mvwprintw (win, y, *x, "%11s", bw);
    wattroff (win, color->attr | COLOR_PAIR (color->pair->idx));
  }

out:
  *x += DASH_BW_LEN + DASH_SPACE;
}

/* render dashboard percent */
static void
render_percent (GDashModule * data, GDashRender render, int *x)
{
  GColorItem item = COLOR_MTRC_PERC;
  GColors *color;
  WINDOW *win = render.win;

  char *percent;
  int y = render.y, w = render.w, idx = render.idx, sel = render.sel;
  int len = data->perc_len + 3;

  if (data->module == HOSTS && data->data[idx].is_subitem)
    goto out;

  if (data->max_hits == data->data[idx].metrics->hits)
    item = COLOR_MTRC_PERC_MAX;
  color = get_color_by_item_module (item, data->module);

  if (sel) {
    /* selected state */
    percent = float2str (data->data[idx].metrics->percent, len);
    draw_header (win, percent, "%s%%", y, *x, w, color_selected);
    free (percent);
  } else {
    /* regular state */
    wattron (win, color->attr | COLOR_PAIR (color->pair->idx));
    mvwprintw (win, y, *x, "%*.2f%%", len, data->data[idx].metrics->percent);
    wattroff (win, color->attr | COLOR_PAIR (color->pair->idx));
  }

out:
  *x += len + 1 + DASH_SPACE;
}

/* render dashboard hits */
static void
render_hits (GDashModule * data, GDashRender render, int *x)
{
  GColors *color = get_color_by_item_module (COLOR_MTRC_HITS, data->module);
  WINDOW *win = render.win;

  char *hits;
  int y = render.y, w = render.w, idx = render.idx, sel = render.sel;
  int len = data->hits_len;

  if (data->module == HOSTS && data->data[idx].is_subitem)
    goto out;

  if (sel) {
    /* selected state */
    hits = int2str (data->data[idx].metrics->hits, 0);
    draw_header (win, hits, " %s", y, 0, w, color_selected);
    free (hits);
  } else {
    /* regular state */
    wattron (win, color->attr | COLOR_PAIR (color->pair->idx));
    mvwprintw (win, y, *x, "%d", data->data[idx].metrics->hits);
    wattroff (win, color->attr | COLOR_PAIR (color->pair->idx));
  }

out:
  *x += len + DASH_SPACE;
}

/* render dashboard hits */
static void
render_visitors (GDashModule * data, GDashRender render, int *x)
{
  GColors *color = get_color_by_item_module (COLOR_MTRC_VISITORS, data->module);
  WINDOW *win = render.win;

  char *visitors;
  int y = render.y, w = render.w, idx = render.idx, sel = render.sel;
  int len = data->visitors_len;

  if (data->module == HOSTS && data->data[idx].is_subitem)
    goto out;

  if (sel) {
    /* selected state */
    visitors = int2str (data->data[idx].metrics->visitors, len);
    draw_header (win, visitors, "%s", y, *x, w, color_selected);
    free (visitors);
  } else {
    /* regular state */
    wattron (win, color->attr | COLOR_PAIR (color->pair->idx));
    mvwprintw (win, y, *x, "%*d", len, data->data[idx].metrics->visitors);
    wattroff (win, color->attr | COLOR_PAIR (color->pair->idx));
  }

out:
  *x += len + DASH_SPACE;
}

static void
render_header (WINDOW * win, GDashModule * data, GModule cur_module, int *y)
{
  GColors *(*func) (void);
  char ind;
  char *hd;
  int k, w, h;

  getmaxyx (win, h, w);
  (void) h;

  k = data->module + 1;
  ind = cur_module == data->module ? '>' : ' ';
  func = cur_module == data->module &&
    conf.hl_header ? color_panel_active : color_panel_header;
  hd = xmalloc (snprintf (NULL, 0, "%c %d - %s", ind, k, data->head) + 1);
  sprintf (hd, "%c %d - %s", ind, k, data->head);

  draw_header (win, hd, " %s", (*y), 0, w, func);
  free (hd);

  render_total_label (win, data, (*y), func);
  data->pos_y = (*y);
  (*y)++;
}

static void
render_description (WINDOW * win, GDashModule * data, int *y)
{
  int w, h;

  getmaxyx (win, h, w);
  (void) h;

  draw_header (win, data->desc, " %s", (*y), 0, w, color_panel_desc);

  data->pos_y = (*y);
  (*y)++;
  (*y)++;       /* add empty line underneath description */
}

static void
render_metrics (GDashModule * data, GDashRender render, int expanded)
{
  int x = get_xpos ();
  GModule module = data->module;
  const GOutput *output = output_lookup (module);

  /* basic metrics */
  if (output->hits)
    render_hits (data, render, &x);
  if (output->visitors)
    render_visitors (data, render, &x);
  if (output->percent)
    render_percent (data, render, &x);

  /* render bandwidth if available */
  if (conf.bandwidth && output->bw)
    render_bw (data, render, &x);

  /* render avgts, cumts and maxts if available */
  if (output->avgts && conf.serve_usecs)
    render_avgts (data, render, &x);
  if (output->cumts && conf.serve_usecs)
    render_cumts (data, render, &x);
  if (output->maxts && conf.serve_usecs)
    render_maxts (data, render, &x);

  /* render request method if available */
  if (output->method && conf.append_method)
    render_method (data, render, &x);
  /* render request protocol if available */
  if (output->protocol && conf.append_protocol)
    render_proto (data, render, &x);
  if (output->data)
    render_data (data, render, &x);

  /* skip graph bars if module is expanded and we have sub nodes */
  if ((output->graph && !expanded) || (output->sub_graph && expanded))
    render_bars (data, render, &x);
}

static void
render_data_line (WINDOW * win, GDashModule * data, int *y, int j,
                  GScroll * gscroll)
{
  GDashRender render;
  GModule module = data->module;
  int expanded = 0, sel = 0;
  int w, h;

  getmaxyx (win, h, w);
  (void) h;

  if (gscroll->expanded && module == gscroll->current)
    expanded = 1;

  if (j >= data->idx_data)
    goto out;

  sel = expanded && j == gscroll->module[module].scroll ? 1 : 0;

  render.win = win;
  render.y = *y;
  render.w = w;
  render.idx = j;
  render.sel = sel;

  render_metrics (data, render, expanded);

out:
  (*y)++;
}

static void
print_horizontal_dash (WINDOW * win, int y, int x, int len)
{
  mvwprintw (win, y, x, "%.*s", len, "----------------");
}

static void
lprint_col (WINDOW * win, int y, int *x, int len, const char *fmt,
            const char *str)
{
  GColors *color = get_color (COLOR_PANEL_COLS);

  wattron (win, color->attr | COLOR_PAIR (color->pair->idx));
  mvwprintw (win, y, *x, fmt, str);
  print_horizontal_dash (win, y + 1, *x, len);
  wattroff (win, color->attr | COLOR_PAIR (color->pair->idx));

  *x += len + DASH_SPACE;
}

static void
rprint_col (WINDOW * win, int y, int *x, int len, const char *fmt,
            const char *str)
{
  GColors *color = get_color (COLOR_PANEL_COLS);

  wattron (win, color->attr | COLOR_PAIR (color->pair->idx));
  mvwprintw (win, y, *x, fmt, len, str);
  print_horizontal_dash (win, y + 1, *x, len);
  wattroff (win, color->attr | COLOR_PAIR (color->pair->idx));

  *x += len + DASH_SPACE;
}

static void
render_cols (WINDOW * win, GDashModule * data, int *y)
{
  GModule module = data->module;
  const GOutput *output = output_lookup (module);
  int x = get_xpos ();

  if (data->idx_data == 0 || conf.no_column_names)
    return;

  if (output->hits)
    lprint_col (win, *y, &x, data->hits_len, "%s", MTRC_HITS_LBL);

  if (output->visitors)
    rprint_col (win, *y, &x, data->visitors_len, "%*s",
                MTRC_VISITORS_SHORT_LBL);

  if (output->percent)
    rprint_col (win, *y, &x, data->perc_len + 4, "%*s", "%");

  if (output->bw && conf.bandwidth)
    rprint_col (win, *y, &x, DASH_BW_LEN, "%*s", MTRC_BW_LBL);

  if (output->avgts && conf.serve_usecs)
    rprint_col (win, *y, &x, DASH_SRV_TM_LEN, "%*s", MTRC_AVGTS_LBL);

  if (output->cumts && conf.serve_usecs)
    rprint_col (win, *y, &x, DASH_SRV_TM_LEN, "%*s", MTRC_CUMTS_LBL);

  if (output->maxts && conf.serve_usecs)
    rprint_col (win, *y, &x, DASH_SRV_TM_LEN, "%*s", MTRC_MAXTS_LBL);

  if (output->method && conf.append_method)
    lprint_col (win, *y, &x, data->method_len, "%s", MTRC_METHODS_SHORT_LBL);

  if (output->protocol && conf.append_protocol)
    lprint_col (win, *y, &x, 8, "%s", MTRC_PROTOCOLS_SHORT_LBL);

  if (output->data)
    lprint_col (win, *y, &x, 4, "%s", MTRC_DATA_LBL);
}

/* render dashboard content */
static void
render_content (WINDOW * win, GDashModule * data, int *y, int *offset,
                int *total, GScroll * gscroll)
{
  GModule module = data->module;
  int i, j, size, h, w, data_pos = get_data_pos_rows ();

  getmaxyx (win, h, w);
  (void) w;

  size = data->dash_size;
  for (i = *offset, j = 0; i < size; i++) {
    /* header */
    if ((i % size) == DASH_HEAD_POS) {
      render_header (win, data, gscroll->current, y);
    } else if ((i % size) == DASH_EMPTY_POS && conf.no_column_names) {
      /* if no column names, print panel description */
      render_description (win, data, y);
    } else if ((i % size) == DASH_EMPTY_POS || (i % size) == size - 1) {
      /* blank lines */
      (*y)++;
    } else if ((i % size) == DASH_DASHES_POS && !conf.no_column_names) {
      /* account for already printed dash lines under columns */
      (*y)++;
    } else if ((i % size) == DASH_COLS_POS && !conf.no_column_names) {
      /* column headers lines */
      render_cols (win, data, y);
      (*y)++;
    } else if ((i % size) >= data_pos || (i % size) <= size - 2) {
      /* account for 2 lines at the header and 2 blank lines */
      j = ((i % size) - data_pos) + gscroll->module[module].offset;
      /* actual data */
      render_data_line (win, data, y, j, gscroll);
    } else {
      /* everything else should be empty */
      (*y)++;
    }
    (*total)++;
    if (*y >= h)
      break;
  }
}

/* entry point to render dashboard */
void
display_content (WINDOW * win, GLog * logger, GDash * dash, GScroll * gscroll)
{
  GDashData *idata;
  GModule module;
  float max_percent = 0.0;
  int j, n = 0, valid = 0;

  int y = 0, offset = 0, total = 0;
  int dash_scroll = gscroll->dash;

  werase (win);

  for (module = 0; module < TOTAL_MODULES; module++) {
    n = dash->module[module].idx_data;
    offset = 0;
    for (j = 0; j < dash->module[module].dash_size; j++) {
      if (dash_scroll > total) {
        offset++;
        total++;
      }
    }

    idata = dash->module[module].data;
    valid = logger->valid;
    max_percent = set_percent_data (idata, n, valid);

    dash->module[module].module = module;
    dash->module[module].method_len = get_max_method_len (idata, n);
    dash->module[module].data_len = get_max_data_len (idata, n);
    dash->module[module].hits_len = get_max_hit_len (idata, n);
    dash->module[module].max_hits = get_max_hit (idata, n);
    dash->module[module].perc_len = get_max_perc_len (max_percent);
    dash->module[module].visitors_len = get_max_visitor_len (idata, n);

    render_content (win, &dash->module[module], &y, &offset, &total, gscroll);
  }
  wrefresh (win);
}

/* reset scroll and offset for each module */
void
reset_scroll_offsets (GScroll * gscroll)
{
  int i;
  for (i = 0; i < TOTAL_MODULES; i++) {
    gscroll->module[i].scroll = 0;
    gscroll->module[i].offset = 0;
  }
}

/* compile the regular expression and see if it's valid */
static int
regexp_init (regex_t * regex, const char *pattern)
{
  int y, x, rc;
  char buf[REGEX_ERROR];

  getmaxyx (stdscr, y, x);
  rc = regcomp (regex, pattern, REG_EXTENDED | (find_t.icase ? REG_ICASE : 0));
  /* something went wrong */
  if (rc != 0) {
    regerror (rc, regex, buf, sizeof (buf));
    draw_header (stdscr, buf, "%s", y - 1, 0, x, color_error);
    refresh ();
    return 1;
  }
  return 0;
}

/* set search gscroll */
static void
perform_find_dash_scroll (GScroll * gscroll, GModule module)
{
  int *scrll, *offset;
  int exp_size = get_num_expanded_data_rows ();

  /* reset gscroll offsets if we are changing module */
  if (gscroll->current != module)
    reset_scroll_offsets (gscroll);

  scrll = &gscroll->module[module].scroll;
  offset = &gscroll->module[module].offset;

  (*scrll) = find_t.next_idx;
  if (*scrll >= exp_size && *scrll >= *offset + exp_size)
    (*offset) = (*scrll) < exp_size - 1 ? 0 : (*scrll) - exp_size + 1;

  gscroll->current = module;
  gscroll->dash = module * DASH_COLLAPSED;
  gscroll->expanded = 1;
  find_t.module = module;
}

/* find item within the given sub_list */
static int
find_next_sub_item (GSubList * sub_list, regex_t * regex)
{
  GSubItem *iter;
  int i = 0, rc;

  if (sub_list == NULL)
    goto out;

  for (iter = sub_list->head; iter; iter = iter->next) {
    if (i >= find_t.next_sub_idx) {
      rc = regexec (regex, iter->metrics->data, 0, NULL, 0);
      if (rc == 0) {
        find_t.next_idx++;
        find_t.next_sub_idx = (1 + i);
        return 0;
      }
      find_t.next_idx++;
    }
    i++;
  }
out:
  find_t.next_parent_idx++;
  find_t.next_sub_idx = 0;
  find_t.look_in_sub = 0;

  return 1;
}

/* perform a forward search across all modules */
int
perform_next_find (GHolder * h, GScroll * gscroll)
{
  int y, x, j, n, rc;
  char buf[REGEX_ERROR];
  char *data;
  regex_t regex;
  GModule module;
  GSubList *sub_list;

  getmaxyx (stdscr, y, x);

  if (find_t.pattern == NULL || *find_t.pattern == '\0')
    return 1;

  /* compile and initialize regexp */
  if (regexp_init (&regex, find_t.pattern))
    return 1;

  /* use last find_t.module and start search */
  for (module = find_t.module; module < TOTAL_MODULES; module++) {
    n = h[module].idx;
    for (j = find_t.next_parent_idx; j < n; j++, find_t.next_idx++) {
      data = h[module].items[j].metrics->data;

      rc = regexec (&regex, data, 0, NULL, 0);
      if (rc != 0 && rc != REG_NOMATCH) {
        regerror (rc, &regex, buf, sizeof (buf));
        draw_header (stdscr, buf, "%s", y - 1, 0, x, color_error);
        refresh ();
        regfree (&regex);
        return 1;
      } else if (rc == 0 && !find_t.look_in_sub) {
        find_t.look_in_sub = 1;
        goto found;
      } else {
        sub_list = h[module].items[j].sub_list;
        if (find_next_sub_item (sub_list, &regex) == 0)
          goto found;
      }
    }
    /* reset find */
    find_t.next_idx = 0;
    find_t.next_parent_idx = 0;
    find_t.next_sub_idx = 0;
    if (find_t.module != module) {
      reset_scroll_offsets (gscroll);
      gscroll->expanded = 0;
    }
    if (module == TOTAL_MODULES - 1) {
      find_t.module = 0;
      goto out;
    }
  }

found:
  perform_find_dash_scroll (gscroll, module);
out:
  regfree (&regex);
  return 0;
}

/* render find dialog */
int
render_find_dialog (WINDOW * main_win, GScroll * gscroll)
{
  int y, x, valid = 1;
  int w = FIND_DLG_WIDTH;
  int h = FIND_DLG_HEIGHT;
  char *query = NULL;
  WINDOW *win;

  getmaxyx (stdscr, y, x);

  win = newwin (h, w, (y - h) / 2, (x - w) / 2);
  keypad (win, TRUE);
  wborder (win, '|', '|', '-', '-', '+', '+', '+', '+');
  draw_header (win, FIND_HEAD, " %s", 1, 1, w - 2, color_panel_header);
  mvwprintw (win, 2, 1, " %s", FIND_DESC);

  find_t.icase = 0;
  query = input_string (win, 4, 2, w - 3, "", 1, &find_t.icase);
  if (query != NULL && *query != '\0') {
    reset_scroll_offsets (gscroll);
    reset_find ();
    find_t.pattern = xstrdup (query);
    valid = 0;
  }
  if (query != NULL)
    free (query);
  touchwin (main_win);
  close_win (win);
  wrefresh (main_win);

  return valid;
}

/* add an item from a sub_list to the dashboard */
static void
add_sub_item_to_dash (GDash ** dash, GHolderItem item, GModule module, int *i)
{
  GSubList *sub_list = item.sub_list;
  GSubItem *iter;
  GDashData *idata;

  char *entry;
  int *idx;
  idx = &(*dash)->module[module].idx_data;

  if (sub_list == NULL)
    return;

  for (iter = sub_list->head; iter; iter = iter->next, (*i)++) {
    entry = render_child_node (iter->metrics->data);
    if (!entry)
      continue;

    idata = &(*dash)->module[module].data[(*idx)];
    idata->metrics = new_gmetrics ();

    idata->metrics->visitors = iter->metrics->visitors;
    idata->metrics->bw.sbw = filesize_str (iter->metrics->bw.nbw);
    idata->metrics->data = xstrdup (entry);
    idata->metrics->hits = iter->metrics->hits;
    if (conf.serve_usecs) {
      idata->metrics->avgts.sts = usecs_to_str (iter->metrics->avgts.nts);
      idata->metrics->cumts.sts = usecs_to_str (iter->metrics->cumts.nts);
      idata->metrics->maxts.sts = usecs_to_str (iter->metrics->maxts.nts);
    }

    idata->is_subitem = 1;
    (*idx)++;
    free (entry);
  }
}

/* add a first level item to dashboard */
static void
add_item_to_dash (GDash ** dash, GHolderItem item, GModule module)
{
  GDashData *idata;
  int *idx = &(*dash)->module[module].idx_data;

  idata = &(*dash)->module[module].data[(*idx)];
  idata->metrics = new_gmetrics ();

  idata->metrics->bw.sbw = filesize_str (item.metrics->bw.nbw);
  idata->metrics->data = xstrdup (item.metrics->data);
  idata->metrics->hits = item.metrics->hits;
  idata->metrics->visitors = item.metrics->visitors;

  if (conf.append_method && item.metrics->method)
    idata->metrics->method = item.metrics->method;
  if (conf.append_protocol && item.metrics->protocol)
    idata->metrics->protocol = item.metrics->protocol;
  if (conf.serve_usecs) {
    idata->metrics->avgts.sts = usecs_to_str (item.metrics->avgts.nts);
    idata->metrics->cumts.sts = usecs_to_str (item.metrics->cumts.nts);
    idata->metrics->maxts.sts = usecs_to_str (item.metrics->maxts.nts);
  }

  (*idx)++;
}

/* load holder's data into dashboard */
void
load_data_to_dash (GHolder * h, GDash * dash, GModule module, GScroll * gscroll)
{
  int alloc_size = 0;
  int i, j;

  alloc_size = dash->module[module].alloc_data;
  if (gscroll->expanded && module == gscroll->current)
    alloc_size += h->sub_items_size;

  dash->module[module].alloc_data = alloc_size;
  dash->module[module].data = new_gdata (alloc_size);
  dash->module[module].holder_size = h->holder_size;

  for (i = 0, j = 0; i < alloc_size; i++) {
    if (h->items[j].metrics->data == NULL)
      continue;

    add_item_to_dash (&dash, h->items[j], module);
    if (gscroll->expanded && module == gscroll->current && h->sub_items_size)
      add_sub_item_to_dash (&dash, h->items[j], module, &i);
    j++;
  }
}
