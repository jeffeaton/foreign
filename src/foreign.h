/*
 *  $Id: foreign.h,v 1.3 2002/05/01 21:00:11 hornik Exp $
 *
 *  Common header file for the foreign package for R
 *
 *  Copyright 2000-2000 Saikat DebRoy <saikat@stat.wisc.edu>
 *                      Douglas M. Bates <bates@stat.wisc.edu>,
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be
 *  useful, but WITHOUT ANY WARRANTY; without even the implied
 *  warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
 *  PURPOSE.  See the GNU General Public License for more
 *  details.
 *
 *  You should have received a copy of the GNU General Public
 *  License along with this program; if not, write to the Free
 *  Software Foundation, Inc., 59 Temple Place - Suite 330, Boston,
 *  MA 02111-1307, USA
 *
 */

#ifndef FOREIGN_H
#define FOREIGN_H

#include <R.h>
#include <Rinternals.h>

#define CN_TYPE_BIG     1
#define CN_TYPE_LITTLE  2 
#define CN_TYPE_XPORT   3
#define CN_TYPE_IEEEB   CN_TYPE_BIG
#define CN_TYPE_IEEEL   CN_TYPE_LITTLE

#define BIG 4321
#define LITTLE 1234
#define UNKNOWN 0000

#ifdef WORDS_BIGENDIAN
# define CN_TYPE_NATIVE CN_TYPE_IEEEB
# define endian BIG
#else
# define CN_TYPE_NATIVE CN_TYPE_IEEEL
# define endian LITTLE
#endif /* not WORDS_BIGENDIAN */

typedef int int32;
typedef short int16;

typedef double flt64;
typedef float flt32;

#define FPREP_IEEE754 754
#define FPREP FPREP_IEEE754

#ifdef max
# undef max
#endif
#ifdef min
# undef min
#endif
#define max(a,b) ((a) >= (b) ? (a) : (b))
#define min(a,b) ((a) <= (b) ? (a) : (b))

extern char* R_ExpandFileName(char*);

#endif /* FOREIGN_H */
