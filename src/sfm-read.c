/* PSPP - computes sample statistics.
   Copyright (C) 1997-9, 2000 Free Software Foundation, Inc.
   Written by Ben Pfaff <blp@gnu.org>.
   Modified for R foreign library by Saikat DebRoy <saikat@stat.wisc.edu>.

   This program is free software; you can redistribute it and/or
   modify it under the terms of the GNU General Public License as
   published by the Free Software Foundation; either version 2 of the
   License, or (at your option) any later version.

   This program is distributed in the hope that it will be useful, but
   WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA
   02111-1307, USA. */

#include <stdlib.h>
#include <ctype.h>
#include <errno.h>
#include <float.h>
#include "foreign.h"
#include "avl.h"
#include "file-handle.h"
#include "format.h"
#include "sfm.h"
#include "sfmP.h"
#include "var.h"
#include <R.h>

/* Clamps A to be between B and C. */
#define range(A, B, C)				\
	((A) < (B) ? (B) : ((A) > (C) ? (C) : (A)))

/* Divides nonnegative X by positive Y, rounding up. */
#define DIV_RND_UP(X, Y) 			\
	(((X) + ((Y) - 1)) / (Y))

/* Returns nonnegative difference between {nonnegative X} and {the
   least multiple of positive Y greater than or equal to X}. */
#if __GNUC__ && !__STRICT_ANSI__
#define REM_RND_UP(X, Y)			\
	({					\
	  int rem = (X) % (Y);			\
	  rem ? (Y) - rem : 0;			\
	})
#else
#define REM_RND_UP(X, Y) 			\
	((X) % (Y) ? (Y) - (X) % (Y) : 0)
#endif

/* Rounds X up to the next multiple of Y. */
#define ROUND_UP(X, Y) 				\
	(((X) + ((Y) - 1)) / (Y) * (Y))

/* Rounds X down to the previous multiple of Y. */
#define ROUND_DOWN(X, Y) 			\
	((X) / (Y) * (Y))

#undef DEBUGGING
/*#define DEBUGGING 1*/

static double 
second_lowest_double_val()
{
  /* PORTME: Set the value for second_lowest_value, which is the
     "second lowest" possible value for a double.  This is the value
     for LOWEST on MISSING VALUES, etc. */
#if FPREP == FPREP_IEEE754
#ifdef WORDS_BIGENDIAN
    union {
	unsigned char c[8];
	double d;
    } second_lowest = {{0xff, 0xef, 0xff, 0xff, 0xff, 0xff, 0xff, 0xfe}};
#else
    union {
	unsigned char c[8];
	double d;
    } second_lowest = {{0xfe, 0xff, 0xff, 0xff, 0xff, 0xff, 0xef, 0xff}};
#endif
    return second_lowest.d;
#else /* FPREP != FPREP_IEEE754 */
#error Unknown floating-point representation.
#endif /* FPREP != FPREP_IEEE754 */
}

/* PORTME: This file may require substantial revision for those
   systems that don't meet the typical 32-bit integer/64-bit double
   model.  It's kinda hard to tell without having one of them on my
   desk.  */

/* sfm's file_handle extension. */
struct sfm_fhuser_ext
  {
    FILE *file;			/* Actual file. */
    int opened;			/* Reference count. */

    struct dictionary *dict;	/* File's dictionary. */

    int reverse_endian;		/* 1=file has endianness opposite us. */
    int case_size;		/* Number of `values's per case. */
    long ncases;		/* Number of cases, -1 if unknown. */
    int compressed;		/* 1=compressed, 0=not compressed. */
    double bias;		/* Compression bias, usually 100.0. */
    int weight_index;		/* 0-based index of weighting variable, or -1. */

    /* File's special constants. */
    flt64 sysmis;
    flt64 highest;
    flt64 lowest;

    /* Uncompression buffer. */
    flt64 *buf;			/* Buffer data. */
    flt64 *ptr;			/* Current location in buffer. */
    flt64 *end;			/* End of buffer data. */

    /* Compression instruction octet. */
    unsigned char x[sizeof (flt64)];
    /* Current instruction octet. */
    unsigned char *y;		/* Location in current instruction octet. */
  };

#ifndef Macintosh
static struct fh_ext_class sfm_r_class;
#else
static void sfm_close (struct file_handle * h);
static struct fh_ext_class sfm_r_class =
{
  3,
  "reading as a system file",
  sfm_close,
};
#endif

#if GLOBAL_DEBUGGING
void dump_dictionary (struct dictionary * dict);
#endif

/* Utilities. */

#include "swap_bytes.h"

/* bswap_int32(): Reverse the byte order of 32-bit integer *X. */
#define bswap_int32(x) reverse_int(*x)

/* Reverse the byte order of 64-bit floating point *X. */
#define bswap_flt64(x) reverse_double(*x)

/* Closes a system file after we're done with it. */
static void
sfm_close (struct file_handle * h)
{
  struct sfm_fhuser_ext *ext = h->ext;

  ext->opened--;
  if (!(ext->opened == 0)) error("assert failed : ext->opened == 0");
  Free (ext->buf);
  if (EOF == fclose (ext->file))
    error("%s: Closing system file: %s.", h->fn, strerror (errno));
}

/* Closes a system file if we're done with it. */
void
sfm_maybe_close (struct file_handle *h)
{
  struct sfm_fhuser_ext *ext = h->ext;

  if (ext->opened == 1)
    fh_close_handle (h);
  else
    ext->opened--;
}

/* Dictionary reader. */

static void *bufread (struct file_handle * handle, void *buf, size_t nbytes,
		      size_t minalloc);

static int read_header (struct file_handle * h, struct sfm_read_info * inf);
static int parse_format_spec (struct file_handle * h, int32 s,
			      struct fmt_spec * v, struct variable *vv);
static int read_value_labels (struct file_handle * h, struct variable ** var_by_index);
static int read_variables (struct file_handle * h, struct variable *** var_by_index);
static int read_machine_int32_info (struct file_handle * h, int size, int count);
static int read_machine_flt64_info (struct file_handle * h, int size, int count);
static int read_documents (struct file_handle * h);

/* Displays the message X with corrupt_msg, then jumps to the lossage
   label. */
#define lose(X)					\
	do					\
	  {					\
	    warning X;	 			\
	    goto lossage;			\
	  }					\
	while (0)

/* Calls bufread with the specified arguments, and jumps to lossage if
   the read fails. */
#define assertive_bufread(a,b,c,d)		\
	do					\
	  {					\
	    if (!bufread (a,b,c,d))		\
	      goto lossage;			\
	  }					\
	while (0)

/* Decrements the reference count for value label V.  Destroys the
   value label if the reference count reaches zero. */
void
free_value_label (struct value_label * v)
{
  if (!(v->ref_count >= 1)) error("assert failed : v->ref_count >= 1");
  if (--v->ref_count == 0)
    {
      Free (v->s);
      Free (v);
    }
}

/* Frees value label P.  PARAM is ignored.  Used as a callback with
   avl_destroy(). */
void
free_val_lab (void *p, void *param)
{
  free_value_label ((struct value_label *) p);
}

/* Destroys dictionary D. */
void
free_dictionary (struct dictionary * d)
{
  int i;

  d->n_splits = 0;
  Free (d->splits);
  d->splits = NULL;
  
  if (d->var_by_name)
    avl_destroy (d->var_by_name, NULL);

  for (i = 0; i < d->nvar; i++)
    {
      struct variable *v = d->var[i];

      if (v->val_lab)
	{
	  avl_destroy (v->val_lab, free_val_lab);
	  v->val_lab = NULL;
	}
      if (v->label)
	{
	  Free (v->label);
	  v->label = NULL;
	}
      Free (d->var[i]);
    }
  Free (d->var);

  Free (d->label);
  Free (d->documents);

  Free (d);
}

/* Reads the dictionary from file with handle H, and returns it in a
   dictionary structure.  This dictionary may be modified in order to
   rename, reorder, and delete variables, etc.	*/
struct dictionary *
sfm_read_dictionary (struct file_handle * h, struct sfm_read_info * inf)
{
  /* The file handle extension record. */
  struct sfm_fhuser_ext *ext;

  /* Allows for quick reference to variables according to indexes
     relative to position within a case. */
  struct variable **var_by_index = NULL;

  /* Check whether the file is already open. */
  if (h->class == &sfm_r_class)
    {
      ext = h->ext;
      ext->opened++;
      return ext->dict;
    }
  else if (h->class != NULL)
    {
      error("Cannot read file %s as system file: already opened for %s.",
	    fh_handle_name (h), h->class->name);
    }

#if 0
  warning ("%s: Opening system-file handle %s for reading.",
      fh_handle_filename (h), fh_handle_name (h));
#endif
  
  /* Open the physical disk file. */
  ext = (struct sfm_fhuser_ext *) Calloc(1, struct sfm_fhuser_ext);
  ext->file = fopen (R_ExpandFileName(h->norm_fn), "rb");
  if (ext->file == NULL)
    {
      Free (ext);
      error("An error occurred while opening \"%s\" for reading "
	    "as a system file: %s.", h->fn, strerror (errno));
    }

  /* Initialize the sfm_fhuser_ext structure. */
  h->class = &sfm_r_class;
  h->ext = ext;
  ext->dict = NULL;
  ext->buf = ext->ptr = ext->end = NULL;
  ext->y = ext->x + sizeof ext->x;
  ext->opened = 1;

  /* Default special constants. */
  ext->sysmis = -DBL_MAX;
  ext->highest = DBL_MAX;
  ext->lowest = second_lowest_double_val();

  /* Read the header. */
  if (!read_header (h, inf))
    goto lossage;

  /* Read about the variables. */
  if (!read_variables (h, &var_by_index))
    goto lossage;

  /* Handle weighting. */
  if (ext->weight_index != -1)
    {
      struct variable *wv = var_by_index[ext->weight_index];

      if (wv == NULL)
	lose (("%s: Weighting variable may not be a continuation of "
	       "a long string variable.", h->fn));
      else if (wv->type == ALPHA)
	lose (("%s: Weighting variable may not be a string variable.",
	       h->fn));

      strcpy (ext->dict->weight_var, wv->name);
    }
  else
    ext->dict->weight_var[0] = 0;

  /* Read records of types 3, 4, 6, and 7. */
  for (;;)
    {
      int32 rec_type;

      assertive_bufread(h, &rec_type, sizeof rec_type, 0);
      if (ext->reverse_endian)
	bswap_int32 (&rec_type);

      switch (rec_type)
	{
	case 3:
	  if (!read_value_labels (h, var_by_index))
	    goto lossage;
	  break;

	case 4:
	  lose (("%s: Orphaned variable index record (type 4).  Type 4 "
		 "records must always immediately follow type 3 records.",
		 h->fn));

	case 6:
	  if (!read_documents (h))
	    goto lossage;
	  break;

	case 7:
	  {
	    struct
	      {
		int32 subtype P;
		int32 size P;
		int32 count P;
	      }
	    data;

	    int skip = 0;

	    assertive_bufread(h, &data, sizeof data, 0);
	    if (ext->reverse_endian)
	      {
		bswap_int32 (&data.subtype);
		bswap_int32 (&data.size);
		bswap_int32 (&data.count);
	      }

	    /*if(data.size != sizeof(int32) && data.size != sizeof(flt64))
	       lose(("%s: Element size in record type 7, subtype %d, is "
	       "not either the size of IN (%d) or OBS (%d); actual value "
	       "is %d.",
	       h->fn, data.subtype, sizeof(int32), sizeof(flt64),
	       data.size)); */

	    switch (data.subtype)
	      {
	      case 3:
		if (!read_machine_int32_info (h, data.size, data.count))
		  goto lossage;
		break;

	      case 4:
		if (!read_machine_flt64_info (h, data.size, data.count))
		  goto lossage;
		break;

	      case 5:
	      case 6:
	      case 11: /* ?? Used by SPSS 8.0. */
		skip = 1;
		break;

	      default:
		warning("%s: Unrecognized record type 7, subtype %d "
			"encountered in system file.", h->fn, data.subtype);
		skip = 1;
	      }

	    if (skip)
	      {
		void *x = bufread (h, NULL, data.size * data.count, 0);
		if (x == NULL)
		  goto lossage;
		Free (x);
	      }
	  }
	  break;

	case 999:
	  {
	    int32 filler;

	    assertive_bufread(h, &filler, sizeof filler, 0);
	    goto break_out_of_loop;
	  }

	default:
	  lose (("%s: Unrecognized record type %d.", h->fn, rec_type));
	}
    }

break_out_of_loop:
  /* Come here on successful completion. */

#if DEBUGGING
  warning ("Read system-file dictionary successfully.");
  dump_dictionary (ext->dict);
#endif
  Free (var_by_index);
  return ext->dict;

lossage:
  /* Come here on unsuccessful completion. */
  
  Free (var_by_index);
  fclose (ext->file);
  if (ext && ext->dict)
    free_dictionary (ext->dict);
  Free (ext);
  h->class = NULL;
  h->ext = NULL;
  error("Error reading system-file header.");
  return NULL;
}

/* Read record type 7, subtype 3. */
static int
read_machine_int32_info (struct file_handle * h, int size, int count)
{
  struct sfm_fhuser_ext *ext = h->ext;

  int32 data[8];
  int file_endian;

  int i;

  if (size != sizeof (int32) || count != 8)
    lose (("%s: Bad size (%d) or count (%d) field on record type 7, "
    "subtype 3.	Expected size %d, count 8.",
	h->fn, size, count, sizeof (int32)));

  assertive_bufread(h, data, sizeof data, 0);
  if (ext->reverse_endian)
    for (i = 0; i < 8; i++)
      bswap_int32 (&data[i]);

  /* PORTME: Check floating-point representation. */
  switch (FPREP)
    {
    case FPREP_IEEE754:
      if (data[4] != 1)
	lose (("%s: Floating-point representation in system file is not "
	       "IEEE-754.  PSPP cannot convert between floating-point "
	       "formats.", h->fn));
      break;
    default:
      if (!(0)) error("assert failed : 0");
    }

  /* PORTME: Check recorded file endianness against intuited file
     endianness. */
  file_endian = endian;
  if (ext->reverse_endian)
    {
      if (file_endian == BIG)
	file_endian = LITTLE;
      else if (file_endian == LITTLE)
	file_endian = BIG;
      else
	if (!(0)) error("assert failed : 0");
    }
  if ((file_endian == BIG) ^ (data[6] == 1))
    lose (("%s: File-indicated endianness (%s) does not match endianness "
	   "intuited from file header (%s).",
	   h->fn, file_endian == BIG ? "big-endian" : "little-endian",
	   data[6] == 1 ? "big-endian" : (data[6] == 2 ? "little-endian"
					  : "unknown")));

  /* PORTME: Character representation code. */
  if (data[7] != 2 && data[7] != 3)
    lose (("%s: File-indicated character representation code (%s) is not "
	   "ASCII.", h->fn,
       data[7] == 1 ? "EBCDIC" : (data[7] == 4 ? "DEC Kanji" : "Unknown")));

  return 1;

lossage:
  return 0;
}

/* Read record type 7, subtype 4. */
static int
read_machine_flt64_info (struct file_handle * h, int size, int count)
{
  struct sfm_fhuser_ext *ext = h->ext;

  flt64 data[3];

  int i;

  if (size != sizeof (flt64) || count != 3)
    lose (("%s: Bad size (%d) or count (%d) field on record type 7, "
	   "subtype 4.	Expected size %d, count 8.",
	   h->fn, size, count, sizeof (flt64)));

  assertive_bufread(h, data, sizeof data, 0);
  if (ext->reverse_endian)
    for (i = 0; i < 3; i++)
      bswap_flt64 (&data[i]);

  if (data[0] != SYSMIS || data[1] != DBL_MAX
      || data[2] != second_lowest_double_val())
    {
      ext->sysmis = data[0];
      ext->highest = data[1];
      ext->lowest = data[2];
      warning("%s: File-indicated value is different from internal value "
	      "for at least one of the three system values.  SYSMIS: "
	      "indicated %g, expected %g; HIGHEST: %g, %g; LOWEST: "
	      "%g, %g.",
	  h->fn, (double) data[0], (double) SYSMIS,
	  (double) data[1], (double) DBL_MAX,
	  (double) data[2], (double) second_lowest_double_val());
    }
  
  return 1;

lossage:
  return 0;
}

static int
read_header (struct file_handle * h, struct sfm_read_info * inf)
{
  struct sfm_fhuser_ext *ext = h->ext;	/* File extension strcut. */
  struct sysfile_header hdr;		/* Disk buffer. */
  struct dictionary *dict;		/* File dictionary. */
  char prod_name[sizeof hdr.prod_name + 1];	/* Buffer for product name. */
  int skip_amt = 0;		        /* Amount of product name to omit. */
  int i;

  /* Create the dictionary. */
  dict = ext->dict = Calloc (1, struct dictionary);
  dict->var = NULL;
  dict->var_by_name = NULL;
  dict->nvar = 0;
  dict->N = 0;
  dict->nval = -1;		/* Unknown. */
  dict->n_splits = 0;
  dict->splits = NULL;
  dict->weight_var[0] = 0;
  dict->weight_index = -1;
  dict->filter_var[0] = 0;
  dict->label = NULL;
  dict->n_documents = 0;
  dict->documents = NULL;

  /* Read header, check magic. */
  assertive_bufread(h, &hdr, sizeof hdr, 0);
  if (0 != strncmp ("$FL2", hdr.rec_type, 4))
    lose (("%s: Bad magic.  Proper system files begin with "
	   "the four characters `$FL2'. This file will not be read.",
	   h->fn));

  /* Check eye-catcher string. */
  memcpy (prod_name, hdr.prod_name, sizeof hdr.prod_name);
  for (i = 0; i < 60; i++)
    if (!isprint ((unsigned char) prod_name[i]))
      prod_name[i] = ' ';
  for (i = 59; i >= 0; i--)
    if (!isgraph ((unsigned char) prod_name[i]))
      {
	prod_name[i] = '\0';
	break;
      }
  prod_name[60] = '\0';
  
  {
#define N_PREFIXES 2
    static const char *prefix[N_PREFIXES] =
      {
	"@(#) SPSS DATA FILE",
	"SPSS SYSTEM FILE.",
      };

    int i;

    for (i = 0; i < N_PREFIXES; i++)
      if (!strncmp (prefix[i], hdr.prod_name, strlen (prefix[i])))
	{
	  skip_amt = strlen (prefix[i]);
	  break;
	}
  }
  
  /* Check endianness. */
  /* PORTME: endianness. */
  if (hdr.layout_code == 2)
    ext->reverse_endian = 0;
  else
    {
      bswap_int32 (&hdr.layout_code);
      if (hdr.layout_code != 2)
	lose (("%s: File layout code has unexpected value %d.  Value "
	       "should be 2, in big-endian or little-endian format.",
	       h->fn, hdr.layout_code));

      ext->reverse_endian = 1;
      bswap_int32 (&hdr.case_size);
      bswap_int32 (&hdr.compressed);
      bswap_int32 (&hdr.weight_index);
      bswap_int32 (&hdr.ncases);
      bswap_flt64 (&hdr.bias);
    }

  /* Copy basic info and verify correctness. */
  ext->case_size = hdr.case_size;
  if (hdr.case_size <= 0 || ext->case_size > (INT_MAX
					      / (int) sizeof (union value) / 2))
    lose (("%s: Number of elements per case (%d) is not between 1 "
	   "and %d.", h->fn, hdr.case_size, INT_MAX / sizeof (union value) / 2));

  ext->compressed = hdr.compressed;

  ext->weight_index = hdr.weight_index - 1;
  if (hdr.weight_index < 0 || hdr.weight_index > hdr.case_size)
    lose (("%s: Index of weighting variable (%d) is not between 0 "
	   "and number of elements per case (%d).",
	   h->fn, hdr.weight_index, ext->case_size));

  ext->ncases = hdr.ncases;
  if (ext->ncases < -1 || ext->ncases > INT_MAX / 2)
    lose (("%s: Number of cases in file (%ld) is not between -1 and "
	   "%d.", h->fn, (long) ext->ncases, INT_MAX / 2));

  ext->bias = hdr.bias;
  if (ext->bias != 100.0)
    warning("%s: Compression bias (%g) is not the usual "
	    "value of 100.", h->fn, ext->bias);

  /* Make a file label only on the condition that the given label is
     not all spaces or nulls. */
  {
    int i;

    dict->label = NULL;
    for (i = sizeof hdr.file_label - 1; i >= 0; i--)
      if (!isspace ((unsigned char) hdr.file_label[i])
	  && hdr.file_label[i] != 0)
	{
	  dict->label = Calloc (i + 2, char);
	  memcpy (dict->label, hdr.file_label, i + 1);
	  dict->label[i + 1] = 0;
	  break;
	}
  }

  if (inf)
    {
      char *cp;

      memcpy (inf->creation_date, hdr.creation_date, 9);
      inf->creation_date[9] = 0;

      memcpy (inf->creation_time, hdr.creation_time, 8);
      inf->creation_time[8] = 0;

      if (!ext->reverse_endian)
	inf->endianness = endian;
      else
	inf->endianness = endian == BIG ? LITTLE : BIG;

      inf->compressed = hdr.compressed;

      inf->ncases = hdr.ncases;

      for (cp = &prod_name[skip_amt]; cp < &prod_name[60]; cp++)
	if (isgraph ((unsigned char) *cp))
	  break;
      strcpy (inf->product, cp);
    }

  return 1;

lossage:
  return 0;
}

int
cmp_variable (const void *a, const void *b, void *foo)
{
  return strcmp (((struct variable *) a)->name, ((struct variable *) b)->name);
}

/* Reads most of the dictionary from file H; also fills in the
   associated VAR_BY_INDEX array. 

   Note: the dictionary returned by this function has an invalid NVAL
   element, also the VAR[] array does not have the FV and LV elements
   set, however the NV elements *are* set.  This is because the caller
   will probably modify the dictionary before reading it in from the
   file.  Also, the get.* elements are set to appropriate values to
   allow the file to be read.  */
static int
read_variables (struct file_handle * h, struct variable *** var_by_index)
{
  int i;

  struct sfm_fhuser_ext *ext = h->ext;	/* File extension record. */
  struct dictionary *dict = ext->dict;	/* Dictionary being constructed. */
  struct sysfile_variable sv;		/* Disk buffer. */
  int long_string_count = 0;	/* # of long string continuation
				   records still expected. */
  int next_value = 0;		/* Index to next `value' structure. */

  /* Allocate variables. */
  dict->var = Calloc (ext->case_size, struct variable *);
  *var_by_index = Calloc (ext->case_size, struct variable *);

  /* Read in the entry for each variable and use the info to
     initialize the dictionary. */
  for (i = 0; i < ext->case_size; i++)
    {
      struct variable *vv;
      int j;

      assertive_bufread(h, &sv, sizeof sv, 0);

      if (ext->reverse_endian)
	{
	  bswap_int32 (&sv.rec_type);
	  bswap_int32 (&sv.type);
	  bswap_int32 (&sv.has_var_label);
	  bswap_int32 (&sv.n_missing_values);
	  bswap_int32 (&sv.print);
	  bswap_int32 (&sv.write);
	}

      if (sv.rec_type != 2)
	lose (("%s: position %d: Bad record type (%d); "
	       "the expected value was 2.", h->fn, i, sv.rec_type));

      /* If there was a long string previously, make sure that the
	 continuations are present; otherwise make sure there aren't
	 any. */
      if (long_string_count)
	{
	  if (sv.type != -1)
	    lose (("%s: position %d: String variable does not have "
			 "proper number of continuation records.", h->fn, i));

	  (*var_by_index)[i] = NULL;
	  long_string_count--;
	  continue;
	}
      else if (sv.type == -1)
	lose (("%s: position %d: Superfluous long string continuation "
	       "record.", h->fn, i));

      /* Check fields for validity. */
      if (sv.type < 0 || sv.type > 255)
	lose (("%s: position %d: Bad variable type code %d.",
	       h->fn, i, sv.type));
      if (sv.has_var_label != 0 && sv.has_var_label != 1)
	lose (("%s: position %d: Variable label indicator field is not "
	       "0 or 1.", h->fn, i));
      if (sv.n_missing_values < -3 || sv.n_missing_values > 3
	  || sv.n_missing_values == -1)
	lose (("%s: position %d: Missing value indicator field is not "
		     "-3, -2, 0, 1, 2, or 3.", h->fn, i));

      /* Construct internal variable structure, initialize critical bits. */
      vv = (*var_by_index)[i] = dict->var[dict->nvar++] =
	  Calloc (1, struct variable);
      vv->index = dict->nvar - 1;
      vv->foo = -1;
      vv->label = NULL;
      vv->val_lab = NULL;

      /* Copy first character of variable name. */
      if (!isalpha ((unsigned char) sv.name[0])
	  && sv.name[0] != '@' && sv.name[0] != '#')
	lose (("%s: position %d: Variable name begins with invalid "
	       "character.", h->fn, i));
      if (islower ((unsigned char) sv.name[0]))
	warning("%s: position %d: Variable name begins with lowercase letter "
	     "%c.", h->fn, i, sv.name[0]);
      if (sv.name[0] == '#')
	warning("%s: position %d: Variable name begins with octothorpe "
		   "(`#').  Scratch variables should not appear in system "
		   "files.", h->fn, i);
      vv->name[0] = toupper ((unsigned char) (sv.name[0]));

      /* Copy remaining characters of variable name. */
      for (j = 1; j < 8; j++)
	{
	  int c = (unsigned char) sv.name[j];

	  if (isspace (c))
	    break;
	  else if (islower (c))
	    {
	      warning("%s: position %d: Variable name character %d is "
		   "lowercase letter %c.", h->fn, i, j + 1, sv.name[j]);
	      vv->name[j] = toupper ((unsigned char) (c));
	    }
	  else if (isalnum (c) || c == '.' || c == '@'
		   || c == '#' || c == '$' || c == '_')
	    vv->name[j] = c;
	  else
	    lose (("%s: position %d: character `\\%03o' (%c) is not valid in a "
		   "variable name.", h->fn, i, c, c));
	}
      vv->name[j] = 0;

      /* Set type, width, and `left' fields and allocate `value'
	 indices. */
      if (sv.type == 0)
	{
	  vv->type = NUMERIC;
	  vv->width = 0;
	  vv->get.nv = 1;
	  vv->get.fv = next_value++;
	  vv->nv = 1;
	}
      else
	{
	  vv->type = ALPHA;
	  vv->width = sv.type;
	  vv->nv = DIV_RND_UP (vv->width, MAX_SHORT_STRING);
	  vv->get.nv = DIV_RND_UP (vv->width, sizeof (flt64));
	  vv->get.fv = next_value;
	  next_value += vv->get.nv;
	  long_string_count = vv->get.nv - 1;
	}
      vv->left = (vv->name[0] == '#');

      /* Get variable label, if any. */
      if (sv.has_var_label == 1)
	{
	  /* Disk buffer. */
	  int32 len;

	  /* Read length of label. */
	  assertive_bufread(h, &len, sizeof len, 0);
	  if (ext->reverse_endian)
	    bswap_int32 (&len);

	  /* Check len. */
	  if (len < 0 || len > 255)
	    lose (("%s: Variable %s indicates variable label of invalid "
		   "length %d.", h->fn, vv->name, len));

	  /* Read label into variable structure. */
	  vv->label = bufread (h, NULL, ROUND_UP (len, sizeof (int32)), len + 1);
	  if (vv->label == NULL)
	    goto lossage;
	  vv->label[len] = '\0';
	}

      /* Set missing values. */
      if (sv.n_missing_values != 0)
	{
	  flt64 mv[3];

	  if (vv->width > MAX_SHORT_STRING)
	    lose (("%s: Long string variable %s may not have missing "
		   "values.", h->fn, vv->name));

	  assertive_bufread(h, mv, sizeof *mv * abs (sv.n_missing_values), 0);

	  if (ext->reverse_endian && vv->type == NUMERIC)
	    for (j = 0; j < abs (sv.n_missing_values); j++)
	      bswap_flt64 (&mv[j]);

	  if (sv.n_missing_values > 0)
	    {
	      vv->miss_type = sv.n_missing_values;
	      if (vv->type == NUMERIC)
		for (j = 0; j < sv.n_missing_values; j++)
		  vv->missing[j].f = mv[j];
	      else
		for (j = 0; j < sv.n_missing_values; j++)
		  memcpy (vv->missing[j].s, &mv[j], vv->width);
	    }
	  else
	    {
	      int x = 0;

	      if (vv->type == ALPHA)
		lose (("%s: String variable %s may not have missing "
		       "values specified as a range.", h->fn, vv->name));

	      if (mv[0] == ext->lowest)
		{
		  vv->miss_type = MISSING_LOW;
		  vv->missing[x++].f = mv[1];
		}
	      else if (mv[1] == ext->highest)
		{
		  vv->miss_type = MISSING_HIGH;
		  vv->missing[x++].f = mv[0];
		}
	      else
		{
		  vv->miss_type = MISSING_RANGE;
		  vv->missing[x++].f = mv[0];
		  vv->missing[x++].f = mv[1];
		}

	      if (sv.n_missing_values == -3)
		{
		  vv->miss_type += 3;
		  vv->missing[x++].f = mv[2];
		}
	    }
	}
      else
	vv->miss_type = MISSING_NONE;

      if (!parse_format_spec (h, sv.print, &vv->print, vv)
	  || !parse_format_spec (h, sv.write, &vv->write, vv))
	goto lossage;
    }

  /* Some consistency checks. */
  if (long_string_count != 0)
    lose (("%s: Long string continuation records omitted at end of "
	   "dictionary.", h->fn));
  if (next_value != ext->case_size)
    lose (("%s: System file header indicates %d variable positions but "
	   "%d were read from file.", h->fn, ext->case_size, next_value));
  dict->var = Realloc (dict->var, dict->nvar, struct variable *);

  /* Construct AVL tree of dictionary in order to speed up later
     processing and to check for duplicate varnames. */
  dict->var_by_name = avl_create (cmp_variable, NULL);
  for (i = 0; i < dict->nvar; i++)
    if (NULL != avl_insert (dict->var_by_name, dict->var[i]))
      lose (("%s: Duplicate variable name `%s' within system file.",
	     h->fn, dict->var[i]->name));

  return 1;

lossage:
  for (i = 0; i < dict->nvar; i++)
    {
      Free (dict->var[i]->label);
      Free (dict->var[i]);
    }
  Free (dict->var);
  if (dict->var_by_name)
    avl_destroy (dict->var_by_name, NULL);
  Free (dict);
  ext->dict = NULL;

  return 0;
}

/* Translates the format spec from sysfile format to internal
   format. */
static int
parse_format_spec (struct file_handle *h, int32 s, struct fmt_spec *v, struct variable *vv)
{
  if ((size_t) ((s >> 16) & 0xff)
      >= sizeof translate_fmt / sizeof *translate_fmt)
    lose (("%s: Bad format specifier byte (%d).",
	   h->fn, (s >> 16) & 0xff));
  
  v->type = translate_fmt[(s >> 16) & 0xff];
  v->w = (s >> 8) & 0xff;
  v->d = s & 0xff;

  /* FIXME?  Should verify the resulting specifier more thoroughly. */

  if (v->type == -1)
    lose (("%s: Bad format specifier byte (%d).",
	   h->fn, (s >> 16) & 0xff));
  if ((vv->type == ALPHA) ^ ((formats[v->type].cat & FCAT_STRING) != 0))
    lose (("%s: %s variable %s has %s format specifier %s.",
	   h->fn, vv->type == ALPHA ? "String" : "Numeric",
	   vv->name,
	   formats[v->type].cat & FCAT_STRING ? "string" : "numeric",
	   formats[v->type].name));
  return 1;

lossage:
  return 0;
}

/* Reads value labels from sysfile H and inserts them into the
   associated dictionary. */
int
read_value_labels (struct file_handle * h, struct variable ** var_by_index)
{
  struct sfm_fhuser_ext *ext = h->ext;	/* File extension record. */

  flt64 *raw_label = NULL;	/* Array of raw label values. */
  struct value_label **cooked_label = NULL;	/* Array of cooked labels. */
  int32 n_labels;		/* Number of labels. */

  struct variable **var = NULL;	/* Associated variables. */
  int32 n_vars;			/* Number of associated variables. */

  int i;

  /* First step: read the contents of the type 3 record and record its
     contents.	Note that we can't do much with the data since we
     don't know yet whether it is of numeric or string type. */

  /* Read number of labels. */
  assertive_bufread(h, &n_labels, sizeof n_labels, 0);
  if (ext->reverse_endian)
    bswap_int32 (&n_labels);

  /* Allocate memory. */
  raw_label = Calloc (n_labels, flt64);
  cooked_label = Calloc (n_labels, struct value_label *);
  for (i = 0; i < n_labels; i++)
    cooked_label[i] = NULL;

  /* Read each value/label tuple. */
  for (i = 0; i < n_labels; i++)
    {
      flt64 value;
      unsigned char label_len;

      int rem;

      /* Read value, label length. */
      assertive_bufread(h, &value, sizeof value, 0);
      assertive_bufread(h, &label_len, 1, 0);
      memcpy (&raw_label[i], &value, sizeof value);

      /* Read label. */
      cooked_label[i] = Calloc (1, struct value_label);
      cooked_label[i]->s = Calloc (label_len + 1, char);
      assertive_bufread(h, cooked_label[i]->s, label_len, 0);
      cooked_label[i]->s[label_len] = 0;

      /* Skip padding. */
      rem = REM_RND_UP (label_len + 1, sizeof (flt64));
      if (rem)
	assertive_bufread(h, &value, rem, 0);
    }

  /* Second step: Read the type 4 record that has the list of
     variables to which the value labels are to be applied. */

  /* Read record type of type 4 record. */
  {
    int32 rec_type;
    
    assertive_bufread(h, &rec_type, sizeof rec_type, 0);
    if (ext->reverse_endian)
      bswap_int32 (&rec_type);
    
    if (rec_type != 4)
      lose (("%s: Variable index record (type 4) does not immediately "
	     "follow value label record (type 3) as it ought.", h->fn));
  }

  /* Read number of variables associated with value label from type 4
     record. */
  assertive_bufread(h, &n_vars, sizeof n_vars, 0);
  if (ext->reverse_endian)
    bswap_int32 (&n_vars);
  if (n_vars < 1 || n_vars > ext->dict->nvar)
    lose (("%s: Number of variables associated with a value label (%d) "
	   "is not between 1 and the number of variables (%d).",
	   h->fn, n_vars, ext->dict->nvar));

  /* Allocate storage. */
  var = Calloc (n_vars, struct variable *);

  /* Read the list of variables. */
  for (i = 0; i < n_vars; i++)
    {
      int32 var_index;
      struct variable *v;

      /* Read variable index, check range. */
      assertive_bufread(h, &var_index, sizeof var_index, 0);
      if (ext->reverse_endian)
	bswap_int32 (&var_index);
      if (var_index < 1 || var_index > ext->case_size)
	lose (("%s: Variable index associated with value label (%d) is "
	       "not between 1 and the number of values (%d).",
	       h->fn, var_index, ext->case_size));

      /* Make sure it's a real variable. */
      v = var_by_index[var_index - 1];
      if (v == NULL)
	lose (("%s: Variable index associated with value label (%d) refers "
	       "to a continuation of a string variable, not to an actual "
	       "variable.", h->fn, var_index));
      if (v->type == ALPHA && v->width > MAX_SHORT_STRING)
	lose (("%s: Value labels are not allowed on long string variables "
	       "(%s).", h->fn, v->name));

      /* Add it to the list of variables. */
      var[i] = v;
    }

  /* Type check the variables. */
  for (i = 1; i < n_vars; i++)
    if (var[i]->type != var[0]->type)
      lose (("%s: Variables associated with value label are not all of "
	     "identical type.  Variable %s has %s type, but variable %s has "
	     "%s type.", h->fn,
	     var[0]->name, var[0]->type == ALPHA ? "string" : "numeric",
	     var[i]->name, var[i]->type == ALPHA ? "string" : "numeric"));

  /* Create a value_label for each value/label tuple, now that we know
     the desired type. */
  for (i = 0; i < n_labels; i++)
    {
      if (var[0]->type == ALPHA)
	{
	  const int copy_len = min (sizeof (flt64), MAX_SHORT_STRING);
	  memcpy (cooked_label[i]->v.s, (char *) &raw_label[i], copy_len);
	  if (MAX_SHORT_STRING > copy_len)
	    memset (&cooked_label[i]->v.s[copy_len], ' ',
		    MAX_SHORT_STRING - copy_len);
	} else {
	  cooked_label[i]->v.f = raw_label[i];
	  if (ext->reverse_endian)
	    bswap_flt64 (&cooked_label[i]->v.f);
	}
      cooked_label[i]->ref_count = n_vars;
    }

  /* Assign the value_label's to each variable. */
  for (i = 0; i < n_vars; i++)
    {
      struct variable *v = var[i];
      int j;

      /* Create AVL tree if necessary. */
      if (!v->val_lab)
	v->val_lab = avl_create (val_lab_cmp, (void *) (v->width));

      /* Add each label to the variable. */
      for (j = 0; j < n_labels; j++)
	{
	  struct value_label *old = avl_replace (v->val_lab, cooked_label[j]);
	  if (old == NULL)
	    continue;

	  if (var[0]->type == NUMERIC)
	    warning("%s: File contains duplicate label for value %g for "
		 "variable %s.", h->fn, cooked_label[j]->v.f, v->name);
	  else
	    warning("%s: File contains duplicate label for value `%.*s' "
		 "for variable %s.", h->fn, v->width,
		 cooked_label[j]->v.s, v->name);

	  free_value_label (old);
	}
    }

  Free (cooked_label);
  Free (raw_label);
  Free (var);
  return 1;

lossage:
  if (cooked_label)
    for (i = 0; i < n_labels; i++)
      if (cooked_label[i])
	{
	  Free (cooked_label[i]->s);
	  Free (cooked_label[i]);
	}
  Free (raw_label);
  Free (var);
  return 0;
}

/* Reads NBYTES bytes from the file represented by H.  If BUF is
   non-NULL, uses that as the buffer; otherwise allocates at least
   MINALLOC bytes.  Returns a pointer to the buffer on success, NULL
   on failure. */
static void *
bufread (struct file_handle * h, void *buf, size_t nbytes, size_t minalloc)
{
  struct sfm_fhuser_ext *ext = h->ext;

  if (buf == NULL)
    buf = Calloc (max (nbytes, minalloc), char);
  if ((nbytes != 0) && (1 != fread (buf, nbytes, 1, ext->file)))
    {
      if (ferror (ext->file))
	error("%s: Reading system file: %s.", h->fn, strerror (errno));
      else
	error("%s: Unexpected end of file.", h->fn);
      return NULL;
    }
  return buf;
}

/* Reads a document record, type 6, from system file H, and sets up
   the documents and n_documents fields in the associated
   dictionary. */
static int
read_documents (struct file_handle * h)
{
  struct sfm_fhuser_ext *ext = h->ext;
  struct dictionary *dict = ext->dict;
  int32 n_lines;

  if (dict->documents != NULL)
    lose (("%s: System file contains multiple type 6 (document) records.",
	   h->fn));

  assertive_bufread(h, &n_lines, sizeof n_lines, 0);
  dict->n_documents = n_lines;
  if (dict->n_documents <= 0)
    lose (("%s: Number of document lines (%ld) must be greater than 0.",
	   h->fn, (long) dict->n_documents));

  dict->documents = bufread (h, NULL, 80 * n_lines, 0);
  if (dict->documents == NULL)
    return 0;
  return 1;

lossage:
  return 0;
}

#if GLOBAL_DEBUGGING
#define DEBUGGING 1
#include "debug-print.h"
/* Displays dictionary DICT on stdout. */
void
dump_dictionary (struct dictionary * dict)
{
  int i;

  debug_printf (("dictionary:\n"));
  for (i = 0; i < dict->nvar; i++)
    {
      char print[32];
      struct variable *v = dict->var[i];
      int n, j;

      debug_printf (("	 var %s", v->name));
      /*debug_printf (("(indices:%d,%d)", v->index, v->foo));*/
      debug_printf (("(type:%s,%d)", (v->type == NUMERIC ? "num"
				 : (v->type == ALPHA ? ("str" : "!!!")),
		     v->width));
      debug_printf (("(fv:%d,%d)", v->fv, v->nv));
      /*debug_printf (("(get.fv:%d,%d)", v->get.fv, v->get.nv));*/
      debug_printf (("(left:%s)(miss:", v->left ? "left" : "right"));
	      
      switch (v->miss_type)
	{
	case MISSING_NONE:
	  n = 0;
	  debug_printf (("none"));
	  break;
	case MISSING_1:
	  n = 1;
	  debug_printf (("one"));
	  break;
	case MISSING_2:
	  n = 2;
	  debug_printf (("two"));
	  break;
	case MISSING_3:
	  n = 3;
	  debug_printf (("three"));
	  break;
	case MISSING_RANGE:
	  n = 2;
	  debug_printf (("range"));
	  break;
	case MISSING_LOW:
	  n = 1;
	  debug_printf (("low"));
	  break;
	case MISSING_HIGH:
	  n = 1;
	  debug_printf (("high"));
	  break;
	case MISSING_RANGE_1:
	  n = 3;
	  debug_printf (("range+1"));
	  break;
	case MISSING_LOW_1:
	  n = 2;
	  debug_printf (("low+1"));
	  break;
	case MISSING_HIGH_1:
	  n = 2;
	  debug_printf (("high+1"));
	  break;
	default:
	  if (!(0)) error("assert failed : 0");
	}
      for (j = 0; j < n; j++)
	if (v->type == NUMERIC)
	  debug_printf ((",%g", v->missing[j].f));
	else
	  debug_printf ((",\"%.*s\"", v->width, v->missing[j].s));
      strcpy (print, fmt_to_string (&v->print));
      debug_printf ((")(fmt:%s,%s)(lbl:%s)\n",
		     print, fmt_to_string (&v->write),
		     v->label ? v->label : "nolabel"));
    }
}
#endif

/* Data reader. */

/* Reads compressed data into H->BUF and sets other pointers
   appropriately.  Returns nonzero only if both no errors occur and
   data was read. */
static int
buffer_input (struct file_handle * h)
{
  struct sfm_fhuser_ext *ext = h->ext;
  size_t amt;

  if (ext->buf == NULL)
    ext->buf = Calloc (128, flt64);
  amt = fread (ext->buf, sizeof *ext->buf, 128, ext->file);
  if (ferror (ext->file))
    {
      error("%s: Error reading file: %s.", h->fn, strerror (errno));
      return 0;
    }
  ext->ptr = ext->buf;
  ext->end = &ext->buf[amt];
  return amt;
}

/* Reads a single case consisting of compressed data from system file
   H into the array TEMP[] according to dictionary DICT, and returns
   nonzero only if successful. */
/* Data in system files is compressed in the following manner:
   data values are grouped into sets of eight; each of the eight has
   one instruction byte, which are output together in an octet; each
   byte gives a value for that byte or indicates that the value can be
   found following the instructions. */
static int
read_compressed_data (struct file_handle * h, flt64 * temp)
{
  struct sfm_fhuser_ext *ext = h->ext;

  const unsigned char *p_end = ext->x + sizeof (flt64);
  unsigned char *p = ext->y;

  const flt64 *temp_beg = temp;
  const flt64 *temp_end = &temp[ext->case_size];

  for (;;)
    {
      for (; p < p_end; p++)
	switch (*p)
	  {
	  case 0:
	    /* Code 0 is ignored. */
	    continue;
	  case 252:
	    /* Code 252 is end of file. */
	    if (temp_beg != temp)
	      lose (("%s: Compressed data is corrupted.  Data ends "
		     "partway through a case.", h->fn));
	    goto lossage;
	  case 253:
	    /* Code 253 indicates that the value is stored explicitly
	       following the instruction bytes. */
	    if (ext->ptr == NULL || ext->ptr >= ext->end)
	      if (!buffer_input (h))
		{
		  lose (("%s: Unexpected end of file.", h->fn));
		  goto lossage;
		}
	    memcpy (temp++, ext->ptr++, sizeof *temp);
	    if (temp >= temp_end)
	      goto winnage;
	    break;
	  case 254:
	    /* Code 254 indicates a string that is all blanks. */
	    memset (temp++, ' ', sizeof *temp);
	    if (temp >= temp_end)
	      goto winnage;
	    break;
	  case 255:
	    /* Code 255 indicates the system-missing value. */
	    *temp = ext->sysmis;
	    if (ext->reverse_endian)
	      bswap_flt64 (temp);
	    temp++;
	    if (temp >= temp_end)
	      goto winnage;
	    break;
	  default:
	    /* Codes 1 through 251 inclusive are taken to indicate a
	       value of (BYTE - BIAS), where BYTE is the byte's value
	       and BIAS is the compression bias (generally 100.0). */
	    *temp = *p - ext->bias;
	    if (ext->reverse_endian)
	      bswap_flt64 (temp);
	    temp++;
	    if (temp >= temp_end)
	      goto winnage;
	    break;
	  }

      /* We have reached the end of this instruction octet.  Read
	 another. */
      if (ext->ptr == NULL || ext->ptr >= ext->end)
	if (!buffer_input (h))
	  {
	    if (temp_beg != temp)
	      lose (("%s: Unexpected end of file.", h->fn));
	    goto lossage;
	  }
      memcpy (ext->x, ext->ptr++, sizeof *temp);
      p = ext->x;
    }

  /* Not reached. */
  if (!(0)) error("assert failed : 0");

winnage:
  /* We have filled up an entire record.  Update state and return
     successfully. */
  ext->y = ++p;
  return 1;

lossage:
  /* We have been unsuccessful at filling a record, either through i/o
     error or through an end-of-file indication.  Update state and
     return unsuccessfully. */
  return 0;
}

/* Reads one case from system file H into the value array PERM
   according to the instructions given in associated dictionary DICT,
   which must have the get.* elements appropriately set.  Returns
   nonzero only if successful.  */
int
sfm_read_case (struct file_handle * h, union value * perm, struct dictionary * dict)
{
  struct sfm_fhuser_ext *ext = h->ext;

  size_t nbytes;
  flt64 *temp;

  int i;

  /* Make sure the caller remembered to finish polishing the
     dictionary returned by sfm_read_dictionary(). */
  if (!(dict->nval > 0)) error("assert failed : dict->nval > 0");

  /* The first concern is to obtain a full case relative to the data
     file.  (Cases in the data file have no particular relationship to
     cases in the active file.) */
  nbytes = sizeof *temp * ext->case_size;
  temp = Calloc(ext->case_size, flt64);

  if (ext->compressed == 0)
    {
      size_t amt = fread (temp, 1, nbytes, ext->file);

      if (amt != nbytes)
	{
	  if (ferror (ext->file))
	    error("%s: Reading system file: %s.", h->fn, strerror (errno));
	  else if (amt != 0)
	    error("%s: Partial record at end of system file.", h->fn);
	  goto lossage;
	}
    }
  else if (!read_compressed_data (h, temp))
    goto lossage;

  /* Translate a case in data file format to a case in active file
     format. */
  for (i = 0; i < dict->nvar; i++)
    {
      struct variable *v = dict->var[i];

      if (v->get.fv == -1)
	continue;
      
      if (v->type == NUMERIC)
	{
	  flt64 src = temp[v->get.fv];
	  if (ext->reverse_endian)
	    bswap_flt64 (&src);
	  perm[v->fv].f = src == ext->sysmis ? NA_REAL : src;
	}
      else
	memcpy (perm[v->fv].c, &temp[v->get.fv], v->width);
    }

  Free (temp);
  return 1;

lossage:
  Free (temp);
  return 0;
}

#ifndef Macintosh
static struct fh_ext_class sfm_r_class =
{
  3,
  "reading as a system file",
  sfm_close,
};
#endif
