/* simple gif encoder
 * Copyright (C) 2005 Benjamin Otte <otte@gnome.org
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#include <gtk/gtk.h>
#include <string.h>
#include "gifenc.h"
#include "gifenc-readbits.h"

void _gifenc_write (Gifenc *enc, guint8 *data, guint len);

/*** GENERAL ***/

void
gifenc_palette_free (GifencPalette *palette)
{
  g_return_if_fail (palette != NULL);

  if (palette->free)
    palette->free (palette->data);
  g_free (palette);
}

guint
gifenc_palette_get_alpha_index (const GifencPalette *palette)
{
  g_return_val_if_fail (palette != NULL, 0);
  g_return_val_if_fail (palette->alpha, 0);

  return palette->num_colors;
}

/*** SIMPLE ***/

static guint
gifenc_palette_simple_lookup (gpointer data, guint color, guint *resulting_color)
{
  color &= 0xC0C0C0;
  *resulting_color = color + 0x202020;
  return ((color >> 18) & 0x30) |
	 ((color >> 12) & 0xC) |
	 ((color >> 6) & 0x3);
}

static void
gifenc_palette_simple_write (gpointer unused, guint byte_order, Gifenc *enc)
{
  guint8 data[3];
  guint r, g, b;

  for (r = 0; r < 4; r++) {
    for (g = 0; g < 4; g++) {
      for (b = 0; b < 4; b++) {
	if (byte_order == G_LITTLE_ENDIAN) {
	  data[0] = (b << 6) + (1 << 5);
	  data[1] = (g << 6) + (1 << 5);
	  data[2] = (r << 6) + (1 << 5);
	} else {
	  data[0] = (r << 6) + (1 << 5);
	  data[1] = (g << 6) + (1 << 5);
	  data[2] = (b << 6) + (1 << 5);
	}
	_gifenc_write (enc, data, 3);
      }
    }
  }
}

GifencPalette *
gifenc_palette_get_simple (guint byte_order, gboolean alpha)
{
  GifencPalette *palette;

  g_return_val_if_fail (byte_order == G_LITTLE_ENDIAN || byte_order == G_BIG_ENDIAN, NULL);
  
  palette = g_new (GifencPalette, 1);

  palette->alpha = alpha;
  palette->num_colors = 64;
  palette->byte_order = byte_order;
  palette->data = (gpointer) (alpha ? 0x1 : 0x0);
  palette->lookup = gifenc_palette_simple_lookup;
  palette->write = gifenc_palette_simple_write;
  palette->free = NULL;

  return palette;
}

/*** OCTREE QUANTIZATION ***/

/* maximum number of leaves before starting color reduction */
#define MAX_LEAVES (12000)
/* maximum number of leaves before stopping a running color reduction */
#define STOP_LEAVES (MAX_LEAVES >> 2)

typedef struct _GifencOctree GifencOctree;
struct _GifencOctree {
  GifencOctree *	children[8];	/* children nodes or NULL */
  guint			level;		/* how deep in tree are we? */
  guint			red;		/* sum of all red pixels */
  guint			green;		/* sum of green pixels */
  guint			blue;		/* sum of blue pixels */
  guint			count;		/* amount of pixels at this node */
  guint			color;		/* representations (depending on value):
					   -1: random non-leaf node 
					   -2: root node
					   0x1000000: leaf node with undefined color
					   0-0xFFFFFF: leaf node with defined color */
  guint			id;		/* color index */
};					  
typedef struct {
  GifencOctree *	tree;
  GSList *		non_leaves;
  guint			num_leaves;
} OctreeInfo;
#define OCTREE_IS_LEAF(tree) ((tree)->color <= 0x1000000)

static GifencOctree *
gifenc_octree_new (void)
{
  GifencOctree *ret = g_new0 (GifencOctree, 1);

  ret->color = (guint) -1;
  return ret;
}

static void
gifenc_octree_free (gpointer data)
{
  GifencOctree *tree = data;
  guint i;
  
  for (i = 0; i < 8; i++) {
    if (tree->children[i])
      gifenc_octree_free (tree->children[i]);
  }
  g_free (tree);
}

#define PRINT_NON_LEAVES 1
static void
gifenc_octree_print (GifencOctree *tree, guint flags)
{
#define FLAG_SET(flag) (flags & (flag))
  if (OCTREE_IS_LEAF (tree)) {
    g_print ("%*s %6d %2X-%2X-%2X\n", tree->level * 2, "", tree->count, 
	tree->red / tree->count, tree->green / tree->count, tree->blue / tree->count);
  } else {
    guint i;
    if (FLAG_SET(PRINT_NON_LEAVES))
      g_print ("%*s %6d\n", tree->level * 2, "", tree->count);
    g_assert (tree->red == 0);
    g_assert (tree->green == 0);
    g_assert (tree->blue == 0);
    for (i = 0; i < 8; i++) {
      if (tree->children[i])
	gifenc_octree_print (tree->children[i], flags);
    }
  }
#undef FLAG_SET
}

static guint
color_to_index (guint color, guint index)
{
  guint ret;

  g_assert (index < 8);

  color >>= (7 - index);
  ret = (color & 0x10000) ? 4 : 0;
  if (color & 0x100)
    ret += 2;
  if (color & 0x1)
    ret ++;
  return ret;
}

static void
gifenc_octree_add_one (GifencOctree *tree, guint color)
{
  tree->red += (color >> 16) & 0xFF;
  tree->green += (color >> 8) & 0xFF;
  tree->blue += color & 0xFF;
}

static void
gifenc_octree_add_color (OctreeInfo *info, guint color)
{
  guint index;
  GifencOctree *tree = info->tree;

  for (;;) {
    tree->count++;
    if (tree->level == 8 || OCTREE_IS_LEAF (tree)) {
      if (tree->color < 0x1000000 && tree->color != color) {
	GifencOctree *new = gifenc_octree_new ();
	new->level = tree->level + 1;
	new->count = tree->count - 1;
	new->red = tree->red; tree->red = 0;
	new->green = tree->green; tree->green = 0;
	new->blue = tree->blue; tree->blue = 0;
	new->color = tree->color; tree->color = (guint) -1;
	index = color_to_index (new->color, tree->level);
	tree->children[index] = new;
	info->non_leaves = g_slist_prepend (info->non_leaves, tree);
      } else {
	gifenc_octree_add_one (tree, color);
	return;
      }
    } 
    index = color_to_index (color, tree->level);
    if (tree->children[index]) {
      tree = tree->children[index];
    } else {
      GifencOctree *new = gifenc_octree_new ();
      new->level = tree->level + 1;
      gifenc_octree_add_one (new, color);
      new->count = 1;
      new->color = color;
      tree->children[index] = new;
      info->num_leaves++;
      return;
    }
  }
}

static int
octree_compare_count (gconstpointer a, gconstpointer b)
{
  return ((const GifencOctree *) a)->count - ((const GifencOctree *) b)->count;
}

static void
gifenc_octree_reduce_one (OctreeInfo *info, GifencOctree *tree)
{
  guint i;

  g_assert (!OCTREE_IS_LEAF (tree));
  for (i = 0; i < 8; i++) {
    if (!tree->children[i])
      continue;
    g_assert (OCTREE_IS_LEAF (tree->children[i]));
    tree->red += tree->children[i]->red;
    tree->green += tree->children[i]->green;
    tree->blue += tree->children[i]->blue;
    gifenc_octree_free (tree->children[i]);
    tree->children[i] = NULL;
    info->num_leaves--;
  }
  tree->color = 0x1000000;
  info->num_leaves++;
  info->non_leaves = g_slist_remove (info->non_leaves, tree);
}

static void
gifenc_octree_reduce_colors (OctreeInfo *info, guint stop)
{
  info->non_leaves = g_slist_sort (info->non_leaves, octree_compare_count);
  //g_print ("reducing %u leaves (%u non-leaves)\n", info->num_leaves, 
  //    g_slist_length (info->non_leaves));
  while (info->num_leaves > stop) {
    gifenc_octree_reduce_one (info, info->non_leaves->data);
  }
  //g_print (" ==> to %u leaves\n", info->num_leaves);
}

static guint
gifenc_octree_finalize (GifencOctree *tree, guint start_id)
{
  if (OCTREE_IS_LEAF (tree)) {
    if (tree->color > 0xFFFFFF)
      tree->color = 
	((tree->red / tree->count) << 16) |
	((tree->green / tree->count) << 8) |
	(tree->blue / tree->count);
    tree->id = start_id;
    return tree->id + 1;
  } else {
    guint i;
    for (i = 0; i < 8; i++) {
      if (tree->children[i])
	start_id = gifenc_octree_finalize (tree->children[i], start_id);
    }
    return start_id;
  }
  g_assert_not_reached ();
  return 0;
}

static guint
gifenc_octree_lookup (gpointer data, guint color, guint *looked_up_color)
{
  GifencOctree *tree = data;
  guint idx;

  if (OCTREE_IS_LEAF (tree)) {
    *looked_up_color = tree->color;
    return tree->id;
  } 
  idx = color_to_index (color, tree->level);
  if (tree->children[idx] == NULL) {
    static const guint order[8][7] = {
      { 2, 1, 4, 3, 6, 5, 7 },
      { 3, 0, 5, 2, 7, 4, 6 },
      { 0, 3, 6, 1, 4, 7, 5 },
      { 1, 2, 7, 6, 5, 0, 4 },
      { 6, 5, 0, 7, 2, 1, 3 },
      { 7, 4, 1, 6, 3, 0, 2 },
      { 4, 7, 2, 5, 0, 3, 1 },
      { 5, 6, 3, 4, 1, 2, 0 }
    };
    guint i, tmp;
    for (i = 0; i < 7; i++) {
      tmp = order[idx][i];
      if (!tree->children[tmp])
	continue;
      /* make selection smarter, like using closest match */
      return gifenc_octree_lookup (
	  tree->children[tmp],
	  color, looked_up_color);
    }
    g_assert_not_reached ();
  }
  return gifenc_octree_lookup (
      tree->children[idx],
      color, looked_up_color);
}

static void
gifenc_octree_write (gpointer data, guint byte_order, Gifenc *enc)
{
  GifencOctree *tree = data;
#if G_BYTE_ORDER == G_BIG_ENDIAN
#define WRITE_DATA(data) _gifenc_write (enc, ((void *) (data)) + 1, 3);
#elif G_BYTE_ORDER == G_LITTLE_ENDIAN
#define WRITE_DATA(data) _gifenc_write (enc, (void *) (data), 3);
#endif
  if (OCTREE_IS_LEAF (tree)) {
    if (byte_order == G_BIG_ENDIAN) {
      WRITE_DATA (&tree->color);
    } else {
      guint data[3];
      data[0] = tree->color & 0xFF;
      data[1] = (tree->color & 0xFF00) >> 8;
      data[2] = (tree->color & 0xFF0000) >> 16;
      WRITE_DATA (data);
    }
  } else {
    guint i;
    for (i = 0; i < 8; i++) {
      if (tree->children[i])
	gifenc_octree_write (tree->children[i], byte_order, enc);
    }
  }
#undef WRITE_DATA
}
  
GifencPalette *
gifenc_quantize_image (const guint8 *data, guint width, guint height, guint bpp, 
    guint rowstride, gboolean alpha, gint byte_order, guint max_colors)
{
  guint x, y;
  const guint8 *row;
  OctreeInfo info = { NULL, NULL, 0 };
  GifencPalette *palette;
  
  g_return_val_if_fail (width * height <= (G_MAXUINT >> 8), NULL);

  info.tree = gifenc_octree_new ();
  info.tree->color = (guint) -2; /* special node */
  
  for (y = 0; y < height; y++) {
    row = data;
    for (x = 0; x < width; x++) {
      guint color;
      GIFENC_READ_TRIPLET (color, row);
      gifenc_octree_add_color (&info, color);
      row += bpp;
    }
    //if (info.num_leaves > MAX_LEAVES)
    //  gifenc_octree_reduce_colors (&info, STOP_LEAVES);
    data += rowstride;
  }
  //gifenc_octree_print (info.tree, 1);
  gifenc_octree_reduce_colors (&info, max_colors - (alpha ? 1 : 0));
  gifenc_octree_finalize (info.tree, 0);

  //gifenc_octree_print (info.tree, 1);
  g_print ("total: %u colors (%u non-leaves)\n", info.num_leaves, 
      g_slist_length (info.non_leaves));

  palette = g_new (GifencPalette, 1);
  palette->alpha = alpha;
  palette->num_colors = info.num_leaves;
  palette->byte_order = byte_order;
  palette->data = info.tree;
  palette->lookup = gifenc_octree_lookup;
  palette->write = gifenc_octree_write;
  palette->free = gifenc_octree_free;
  g_slist_free (info.non_leaves);

  return (GifencPalette *) palette;
}

#ifdef TEST_QUANTIZE

int
main (int argc, char **argv)
{
  GError *error = NULL;
  GdkPixbuf *pixbuf;
  guint8 *image;
  GifencPalette *palette;
  Gifenc *enc;
  
  gtk_init (&argc, &argv);

  pixbuf = gdk_pixbuf_new_from_file (argc > 1 ? argv[1] : "/root/rachael.jpg", &error);
  if (error)
    g_printerr ("error: %s\n", error->message);
  palette = gifenc_quantize_image (gdk_pixbuf_get_pixels (pixbuf), 
      gdk_pixbuf_get_width (pixbuf), gdk_pixbuf_get_height (pixbuf), 
      gdk_pixbuf_get_has_alpha (pixbuf) ? 4 : 3,
      gdk_pixbuf_get_rowstride (pixbuf), gdk_pixbuf_get_has_alpha (pixbuf), 
      G_BIG_ENDIAN, 255);
  
  image = gifenc_dither_pixbuf (pixbuf, palette);
  enc = gifenc_open (gdk_pixbuf_get_width (pixbuf), gdk_pixbuf_get_height (pixbuf), "test.gif");
  gifenc_set_palette (enc, palette);
  gifenc_add_image (enc, 0, 0, gdk_pixbuf_get_width (pixbuf), 
      gdk_pixbuf_get_height (pixbuf), 0, image, gdk_pixbuf_get_width (pixbuf));
  g_free (image);
  g_object_unref (pixbuf);
  gifenc_close (enc);

  return 0;
}

#endif

