/***********************************************************************
 *                                                                     *
 * $Id: hpgspcl.c 539 2010-06-10 13:36:12Z hpgs $
 *                                                                     *
 * hpgs - HPGl Script, a hpgl/2 interpreter, which uses a Postscript   *
 *        API for rendering a scene and thus renders to a variety of   *
 *        devices and fileformats.                                     *
 *                                                                     *
 * (C) 2004-2009 ev-i Informationstechnologie GmbH  http://www.ev-i.at *
 *                                                                     *
 * Author: Wolfgang Glas                                               *
 *                                                                     *
 *  hpgs is free software; you can redistribute it and/or              *
 * modify it under the terms of the GNU Lesser General Public          *
 * License as published by the Free Software Foundation; either        *
 * version 2.1 of the License, or (at your option) any later version.  *
 *                                                                     *
 * hpgs is distributed in the hope that it will be useful,             *
 * but WITHOUT ANY WARRANTY; without even the implied warranty of      *
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU   *
 * Lesser General Public License for more details.                     *
 *                                                                     *
 * You should have received a copy of the GNU Lesser General Public    *
 * License along with this library; if not, write to the               *
 * Free Software  Foundation, Inc., 59 Temple Place, Suite 330,        *
 * Boston, MA  02111-1307  USA                                         *
 *                                                                     *
 ***********************************************************************
 *                                                                     *
 * The implementation of the PCL interpreter.                          *
 *                                                                     *
 ***********************************************************************/

#include <hpgsreader.h>
#include <math.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#if defined ( __MINGW32__ ) || defined ( _MSC_VER )
#include<malloc.h>
#else
#include<alloca.h>
#endif

//#define HPGS_PCL_DEBUG
//#define HPGS_PCL_DEBUG_MODE10
//#define HPGS_PCL_DEBUG_BLOCK_MODES

#define MK_PCL_CMD(a,b,c) ((a)<<16 | (b)<<8 | (c))

#define PCL_RASTER_CLEAR_FORCE   2
#define PCL_RASTER_CLEAR_IMAGE   1
#define PCL_RASTER_CLEAR_PARTIAL 0

static void pcl_clear_raster_data(hpgs_reader *reader, int mode)
{
  int i;

  switch (mode)
    {
    case PCL_RASTER_CLEAR_FORCE:
      reader->pcl_raster_presentation = 0;
      reader->pcl_raster_src_width = 0;
      reader->pcl_raster_src_height = 0;
      reader->pcl_raster_dest_width = 0;
      reader->pcl_raster_dest_height = 0;
      reader->pcl_raster_planes = 1;

    case PCL_RASTER_CLEAR_IMAGE:
      reader->pcl_raster_mode = -1;
      reader->pcl_raster_compression  = 0;
    }

  reader->pcl_raster_y_offset  = 0;
  reader->pcl_raster_plane  = 0;
  reader->pcl_raster_line = 0;

  for (i=0;i<8;++i)
    if (reader->pcl_raster_data[i])
      {
        free(reader->pcl_raster_data[i]);
        reader->pcl_raster_data[i]=0;
      }
      
  reader->pcl_raster_data_size = 0;

  if (reader->pcl_image)
    hpgs_image_destroy(reader->pcl_image);

  reader->pcl_image=0;
}

static hpgs_bool is_pcl_compression_supported (int compression, hpgs_bool plane)
{
  return
    compression == 0 ||
    compression == 1 ||
    compression == 2 ||
    compression == 3 ||
    compression == 9 ||
    compression == 10 ||
    (!plane && (compression == 4 ||
                compression == 5   ));
}

// uncompressed raster line.
static int pcl_ignore_raster_data(hpgs_reader *reader,int data_len)
{
  int i;

  for (i=0;i<data_len;++i)
    {
      reader->last_byte = hpgs_getc(reader->in);
      if (reader->last_byte == EOF)
	return -1;
    }

  return data_len;
}

// uncompressed raster line.
static int pcl_read_raster_0(hpgs_reader *reader,int data_len)
{
  unsigned char *data = reader->pcl_raster_data[reader->pcl_raster_plane];
  int i;

  for (i=0;i<data_len;++i)
    {
      reader->last_byte = hpgs_getc(reader->in);
      if (reader->last_byte == EOF)
	return -1;
      
      if (i<reader->pcl_raster_data_size)
        data[i] = reader->last_byte;
    }

  if (data_len < reader->pcl_raster_data_size)
    memset(data+data_len,0,reader->pcl_raster_data_size-data_len);

  return 0;
}

// run length encoding.
static int pcl_read_raster_1(hpgs_reader *reader,int data_len)
{
  unsigned char *data = reader->pcl_raster_data[reader->pcl_raster_plane];
  int i_in=0,i=0;

  while (i_in < data_len)
    {
      int count;
      reader->last_byte = hpgs_getc(reader->in);
      if (reader->last_byte == EOF)
	return -1;

      if (++i_in >= data_len) break;
      count = reader->last_byte + 1;

      reader->last_byte = hpgs_getc(reader->in);
      if (reader->last_byte == EOF)
	return -1;

      ++i_in;

      if (i+count > reader->pcl_raster_data_size)
        count = reader->pcl_raster_data_size-i;
      
      if (count > 0)
        {
          memset(data+i,reader->last_byte,count);
          i+=count;
        }
    }

  if (i < reader->pcl_raster_data_size)
    memset(data+i,0,reader->pcl_raster_data_size-i);

  return 0;
}

// tiff v4.0.
static int pcl_read_raster_2(hpgs_reader *reader,int data_len)
{
  unsigned char *data = reader->pcl_raster_data[reader->pcl_raster_plane];
  int i_in=0,i=0;

  while (i_in < data_len)
    {
      int control;
      reader->last_byte = hpgs_getc(reader->in);
      if (reader->last_byte == EOF)
	return -1;

      if (++i_in >= data_len) break;
      control = reader->last_byte;

      if (control < 128)
        {
          while (control >= 0 && i_in < data_len)
            {
              reader->last_byte = hpgs_getc(reader->in);
              if (reader->last_byte == EOF)
                return -1;

              --control;
              if (i < reader->pcl_raster_data_size)
                data[i] = reader->last_byte;
              ++i;
              ++i_in;
            }
        }
      else if (control > 128)
        {
          int count = 257 - control;

          reader->last_byte = hpgs_getc(reader->in);
          if (reader->last_byte == EOF)
            return -1;

          ++i_in;
          
          if (i+count > reader->pcl_raster_data_size)
            count = reader->pcl_raster_data_size-i;

          if (count > 0)
            {
              memset(data+i,reader->last_byte,count);
              i+=count;
            }
        }
    }

  if (i < reader->pcl_raster_data_size)
    memset(data+i,0,reader->pcl_raster_data_size-i);

  return 0;
}

// delta row
static int pcl_read_raster_3(hpgs_reader *reader,int data_len)
{
  unsigned char *data = reader->pcl_raster_data[reader->pcl_raster_plane];
  int i_in=0,i=0;

  while (i_in < data_len)
    {
      int count;
      int offset;

      reader->last_byte = hpgs_getc(reader->in);
      if (reader->last_byte == EOF)
	return -1;

      if (++i_in >= data_len) break;
      count = (reader->last_byte >> 5) + 1;
      offset = reader->last_byte & 0x1f;

      if (offset == 0x1f)
        {
          do
            {
              reader->last_byte = hpgs_getc(reader->in);
              if (reader->last_byte == EOF)
                return -1;

              ++i_in;
              offset += reader->last_byte;
            }
          while (reader->last_byte == 0xff && i_in < data_len);

          if (i_in >= data_len) break;
        }

      i += offset;

      do
        {
          reader->last_byte = hpgs_getc(reader->in);
          if (reader->last_byte == EOF)
            return -1;

          ++i_in;

          if (i < reader->pcl_raster_data_size)
            data[i] = reader->last_byte;

          ++i;
          --count;
        }
      while (count && i_in < data_len);
    }

  return 0;
}

// Block-based unencoded (Compression mode 4).
static int pcl_read_raster_4_header(hpgs_reader *reader,int data_len)
{
  unsigned int numberOfPixelsPerRow = 0;
  unsigned int b0, b1, b2, b3;
  int nbBytesLeft;
  int nbBytesPerRow;
  int nbRows;

  reader->last_byte = hpgs_getc(reader->in);
  if (reader->last_byte == EOF) return -1;
  b3 = (unsigned int) reader->last_byte;

  reader->last_byte = hpgs_getc(reader->in);
  if (reader->last_byte == EOF) return -1;
  b2 = (unsigned int) reader->last_byte;

  reader->last_byte = hpgs_getc(reader->in);
  if (reader->last_byte == EOF) return -1;
  b1 = (unsigned int) reader->last_byte;

  reader->last_byte = hpgs_getc(reader->in);
  if (reader->last_byte == EOF) return -1;
  b0 = (unsigned int) reader->last_byte;
  
  nbBytesLeft = data_len - 4;
  
  numberOfPixelsPerRow = (unsigned int)(((b3<<24)&0xFF000000) | ((b2<<16)&0xFF0000) | ((b1<<8)&0xFF00) | (b0&0xFF));
  nbBytesPerRow = (numberOfPixelsPerRow+7)/8;
  
  nbRows = nbBytesLeft / nbBytesPerRow;
  
#ifdef HPGS_PCL_DEBUG
      hpgs_log("pcl_read_raster_4_header: nbBytesLeft,numberOfPixelsPerRow,nbBytesPerRow,nbRows =%d,%d,%d,%d.\n",
               nbBytesLeft,numberOfPixelsPerRow,nbBytesPerRow,nbRows);
#endif

  reader->pcl_raster_line = 0;
  reader->pcl_raster_src_height = nbRows;
  reader->pcl_raster_src_width = numberOfPixelsPerRow;
  reader->pcl_raster_data_size = nbBytesPerRow;
  
  reader->pcl_raster_dest_height = reader->pcl_raster_src_height;
  reader->pcl_raster_dest_width =  reader->pcl_raster_src_width;

  return 0;
}

// uncompressed raster line as part of mode 4 block.
static int pcl_read_raster_4(hpgs_reader *reader,int data_len)
{
  unsigned char *data = reader->pcl_raster_data[reader->pcl_raster_plane];
  int i;

  for (i=0;i<data_len && i<reader->pcl_raster_data_size;++i)
    {
      reader->last_byte = hpgs_getc(reader->in);
      if (reader->last_byte == EOF)
	return -1;

      data[i] = reader->last_byte;
    }

  if (data_len < reader->pcl_raster_data_size)
    memset(data+data_len,0,reader->pcl_raster_data_size-data_len);

  return i;
}

// adaptive compression
static int pcl_read_raster_5(hpgs_reader *reader,int data_len, int *repeat)
{
  reader->last_byte = hpgs_getc(reader->in);
  if (reader->last_byte == EOF)
    return -1;
  
  // command byte.
  int cmd = reader->last_byte;

  reader->last_byte = hpgs_getc(reader->in);
  if (reader->last_byte == EOF)
    return -1;
 
  int row_len = reader->last_byte * 256;

  reader->last_byte = hpgs_getc(reader->in);
  if (reader->last_byte == EOF)
    return -1;

  row_len += reader->last_byte;

  switch (cmd)
    {
    case 0:
      if (pcl_read_raster_0(reader,row_len))
        return -1;
      *repeat = 1;
      return row_len+3;

    case 1:
      if (pcl_read_raster_1(reader,row_len))
        return -1;
      *repeat = 1;
      return row_len+3;

    case 2:
      if (pcl_read_raster_2(reader,row_len))
        return -1;
      *repeat = 1;
      return row_len+3;

    case 3:
      if (pcl_read_raster_3(reader,row_len))
        return -1;
      *repeat = 1;
      return row_len+3;

    case 4:
      memset(reader->pcl_raster_data[reader->pcl_raster_plane],0,reader->pcl_raster_data_size);
    case 5:

      *repeat = row_len;
      return 3;

    default:
      return -1;
    }
}

// modified delta row - compression mode 9
static int pcl_read_raster_9(hpgs_reader *reader,int data_len)
{
  unsigned char *data = reader->pcl_raster_data[reader->pcl_raster_plane];
  int i_in=0,i=0;

  while (i_in < data_len)
    {
      int count;
      hpgs_bool more_count;
      int offset;
      hpgs_bool more_offset;
      hpgs_bool comp;
 
      reader->last_byte = hpgs_getc(reader->in);
      if (reader->last_byte == EOF)
	return -1;

      if (++i_in >= data_len) break;

      comp = (reader->last_byte & 0x80) != 0;

      if (comp)
        {
          offset = (reader->last_byte >> 5) & 0x03;
          more_offset = (offset == 0x03);
          count = (reader->last_byte & 0x1f) + 1;
          more_count = (count == 0x20);
        }
      else
        {
          offset = (reader->last_byte >> 3) & 0x0f;
          more_offset = (offset == 0x0f);
          count = (reader->last_byte & 0x07) + 1;
          more_count = (count == 0x08);
        }
      
      while (more_offset && i_in < data_len)
        {
          reader->last_byte = hpgs_getc(reader->in);
          if (reader->last_byte == EOF)
            return -1;

          offset += reader->last_byte;
          more_offset = (reader->last_byte == 0xff);
          
          if (++i_in >= data_len) return 0;
        }

      while (more_count && i_in < data_len)
        {
          reader->last_byte = hpgs_getc(reader->in);
          if (reader->last_byte == EOF)
            return -1;

          count += reader->last_byte;
          more_count = (reader->last_byte == 0xff);
          
          if (++i_in >= data_len) return 0;
        }

      i += offset;

      if (i >= reader->pcl_raster_data_size) break;

      if (comp)
        {
          // run-length encoded replacement.
          int i_rep = 0;

          while (i_rep < count)
            {
              reader->last_byte = hpgs_getc(reader->in);
              if (reader->last_byte == EOF)
                return -1;
              
              if (++i_in >= data_len) return 0;

              int rep_cnt =  reader->last_byte+1;

              reader->last_byte = hpgs_getc(reader->in);
              if (reader->last_byte == EOF)
                return -1;

              ++i_in;

              int rep_val =  reader->last_byte;

              while (rep_cnt-- > 0 && i_rep < count)
                {
                  if (i < reader->pcl_raster_data_size)
                    data[i] = rep_val;

                  ++i;
                  ++i_rep;
                }
            }
        }
      else
        {
          // uncompressed replacement.
          while (count > 0 && i_in < data_len)
            {
              reader->last_byte = hpgs_getc(reader->in);
              if (reader->last_byte == EOF)
                return -1;
              
              if (i < reader->pcl_raster_data_size)
                data[i] = reader->last_byte;

              ++i_in;
              ++i;
              --count;
            }
        }
    }

  return 0;
}

/*
   Decode a 15 bit short rgb delta value from compression mode 10.
 
   delta ... A value with bit 16 (0x8000) set containing
             3 5-bit r,g,b signed delta values. The blue delta
             value must actually be multiplied by 2.

   dr,dg,db ... return  the extracted delta r,g,b values.

 */
static void decode_mode10_short_delta(int delta, int *dr, int *dg, int *db)
{
  *db = (delta & 0x001f);
  if (*db & 0x10) *db |= ~((int)0x0f);
  *db <<= 1;

  *dg = (delta & 0x03e0) >> 5;
  if (*dg & 0x10) *dg |= ~((int)0x0f);

  *dr = (delta & 0x7c00) >> 10;
  if (*dr & 0x10) *dr |= ~((int)0x0f);
}

static int read_mode10_rgb(hpgs_reader *reader, int *i_in, int data_len, hpgs_bool *delta, int *r, int *g, int *b)
{
  if (*i_in >= data_len) return -1;

  reader->last_byte = hpgs_getc(reader->in);
  if (reader->last_byte == EOF)
    return -1;

  if (++(*i_in) >= data_len) return -1;

  int value = reader->last_byte;

  reader->last_byte = hpgs_getc(reader->in);
  if (reader->last_byte == EOF)
    return -1;

  ++(*i_in);

  value = (value << 8) | reader->last_byte;

  if (value & 0x8000) {

    *delta = HPGS_TRUE;
    decode_mode10_short_delta(value,r,g,b);
  
  } else {

   if (*i_in >= data_len) return -1;

   reader->last_byte = hpgs_getc(reader->in);
    if (reader->last_byte == EOF)
      return -1;

    ++(*i_in);

    value = (value << 8) | reader->last_byte;

    *delta = HPGS_FALSE;
    *r = (value >> 15) & 0xff;
    *g = (value >> 7) & 0xff;
    *b = (value << 1) & 0xff;
  }
  return 0;
}

#define HPGS_PCL_MODE10_NEW_COLOR    0x0
#define HPGS_PCL_MODE10_WEST_COLOR   0x1
#define HPGS_PCL_MODE10_NE_COLOR     0x2
#define HPGS_PCL_MODE10_CACHED_COLOR 0x3

/*
   Modified delta row with short rgb deltas - compression mode 10

   This compression mode is documented nowhere in the universe.

   This implementation is a reverse engineering of the encoding algorithm
   contined in the source file dj9xxvip.cpp of HP's hplip Linux Printing
   project. http://hplip.sourceforge.net

 */
static int pcl_read_raster_10(hpgs_reader *reader,int data_len)
{
#ifdef HPGS_PCL_DEBUG_MODE10

  fprintf(stderr,"pcl_raster_line,pcl_raster_data_size,pcl_raster_src_width=%d,%d,%d.\n",
          reader->pcl_raster_line,reader->pcl_raster_data_size,reader->pcl_raster_src_width);

  fprintf(stderr,"mode 10 data (len=%d):\n",data_len);

  size_t pos;
  int iii;

  if (hpgs_istream_tell(reader->in,&pos)) return -1;

  for (iii=0;iii<data_len;++iii) {

    int b = hpgs_getc(reader->in);
    if (b==EOF) { fprintf(stderr,"<EOF>\n"); return -1; }

    fprintf(stderr," %2.2x",b);
  }
  fprintf(stderr,"\n");

  if (hpgs_istream_seek(reader->in,pos)) return -1;
#endif

  unsigned char *data = reader->pcl_raster_data[reader->pcl_raster_plane];
  int i_in=0,i=0;

  int r = 0xff;
  int g = 0xff;
  int b = 0xfe;
  int cr = 0xff;
  int cg = 0xff;
  int cb = 0xfe;

  while (i_in < data_len)
    {
      int count;
      hpgs_bool more_count;
      int offset;
      hpgs_bool more_offset;
      hpgs_bool rle;
      hpgs_bool delta;
      int pixel_source;

      reader->last_byte = hpgs_getc(reader->in);
      if (reader->last_byte == EOF)
	return -1;

      ++i_in;

      rle = (reader->last_byte & 0x80) != 0;

      pixel_source = (reader->last_byte >> 5) & 0x03;
      offset = (reader->last_byte >> 3) & 0x03;
      more_offset = (offset == 0x03);

      if (rle)
        {
          count = (reader->last_byte & 0x07) + 2;
          more_count = (count == 0x09);
        }
      else
        {
          count = (reader->last_byte & 0x07) + 1;
          more_count = (count == 0x08);
        }
      
#ifdef HPGS_PCL_DEBUG_MODE10
      fprintf(stderr,"i,last_byte,pixel_source,rle,offset,more_offset,count,more_count=%d,%2.2x,%d,%d,%d,%d,%d,%d.\n",
              i/3,reader->last_byte,pixel_source,rle,offset,more_offset,count,more_count);
#endif

      while (more_offset && i_in < data_len)
        {
          reader->last_byte = hpgs_getc(reader->in);
          if (reader->last_byte == EOF)
            return -1;

          offset += reader->last_byte;
          more_offset = (reader->last_byte == 0xff);
          
          ++i_in;
        }

#ifdef HPGS_PCL_DEBUG_MODE10
      fprintf(stderr,"offset,count=%d,%d.\n",offset,count);
#endif

      i += offset * 3;

      if (i+2 >= reader->pcl_raster_data_size) break;

      if (rle)
        {
          // run-length encoded replacement.
          int i_rep = 0;

          switch (pixel_source) {

          case HPGS_PCL_MODE10_NEW_COLOR:
            if (read_mode10_rgb(reader,&i_in,data_len,&delta,&r,&g,&b))
              return -1;
            
            if (delta) {
              
              r += data[i];
              g += data[i+1];
              b += data[i+2];
            }
            cr = r; cg = g; cb = b;
            break;

          case HPGS_PCL_MODE10_CACHED_COLOR:
            r = cr; g = cg; b = cb;
            break;

          case HPGS_PCL_MODE10_WEST_COLOR:
            if (i<3) return -1;
            r = data[i-3];
            g = data[i-2];
            b = data[i-1];
            break;
            
          case HPGS_PCL_MODE10_NE_COLOR:
            if (i+5 >= reader->pcl_raster_data_size) return -1;
            r = data[i+3];
            g = data[i+4];
            b = data[i+5];            
            break;
          }
          
          while (i_rep < count)
            {
              if (i < reader->pcl_raster_data_size)
                data[i++] = (unsigned char)r;
              if (i < reader->pcl_raster_data_size)
                data[i++] = (unsigned char)g;
              if (i < reader->pcl_raster_data_size)
                data[i++] = (unsigned char)b;
              
              ++i_rep;

              if (more_count && i_rep >= count && i_in < data_len)
                {
                  reader->last_byte = hpgs_getc(reader->in);
                  if (reader->last_byte == EOF)
                    return -1;
                  
                  count += reader->last_byte;
                  more_count = (reader->last_byte == 0xff);
                  
#ifdef HPGS_PCL_DEBUG_MODE10
                  fprintf(stderr,"i,last_byte,count,more_count=%d,%2.2x,%d,%d.\n",i/3,reader->last_byte,count,more_count);
#endif
                  ++i_in;
                }
            }
        }
      else
        {
          // uncompressed replacement.
          switch (pixel_source) {

          case HPGS_PCL_MODE10_NEW_COLOR:
            if (read_mode10_rgb(reader,&i_in,data_len,&delta,&r,&g,&b))
              return -1;
              
            if (delta) {
              
              r += data[i];
              g += data[i+1];
              b += data[i+2];
            }
             
            cr = r; cg = g; cb = b;

            if (i < reader->pcl_raster_data_size)
              data[i++] = (unsigned char)r;
            if (i < reader->pcl_raster_data_size)
              data[i++] = (unsigned char)g;
            if (i < reader->pcl_raster_data_size)
              data[i++] = (unsigned char)b;
            --count;
            break;

          case HPGS_PCL_MODE10_WEST_COLOR:
            if (i<3) return -1;
            
            if (i < reader->pcl_raster_data_size)
              { data[i] = data[i-3]; ++i; }
            if (i < reader->pcl_raster_data_size)
              { data[i] = data[i-3]; ++i; }
            if (i < reader->pcl_raster_data_size)
              { data[i] = data[i-3]; ++i; }
            --count;
            break;
            
          case HPGS_PCL_MODE10_NE_COLOR:
            if (i+5 >= reader->pcl_raster_data_size) return -1;
            
            { data[i] = data[i+3]; ++i; }
            { data[i] = data[i+3]; ++i; }
            { data[i] = data[i+3]; ++i; }
            --count;
            break;

          case HPGS_PCL_MODE10_CACHED_COLOR:
            if (i < reader->pcl_raster_data_size)
              data[i++] = (unsigned char)cr;
            if (i < reader->pcl_raster_data_size)
              data[i++] = (unsigned char)cg;
            if (i < reader->pcl_raster_data_size)
              data[i++] = (unsigned char)cb;
            --count;
            break;
          }

          while (count > 0 && i_in < data_len)
            {
              if (read_mode10_rgb(reader,&i_in,data_len,&delta,&r,&g,&b))
                return -1;
              
              if (delta) {
              
                r += data[i];
                g += data[i+1];
                b += data[i+2];
              }
              
              if (i < reader->pcl_raster_data_size)
                data[i++] = (unsigned char)r;
              if (i < reader->pcl_raster_data_size)
                data[i++] = (unsigned char)g;
              if (i < reader->pcl_raster_data_size)
                data[i++] = (unsigned char)b;
             
              --count;

              if (more_count && count <= 0 && i_in < data_len)
                {
                  reader->last_byte = hpgs_getc(reader->in);
                  if (reader->last_byte == EOF)
                    return -1;
                  
                  count += reader->last_byte;
                  more_count = (reader->last_byte == 0xff);
                  
#ifdef HPGS_PCL_DEBUG_MODE10
                  fprintf(stderr,"i,last_byte,count,more_count=%d,%2.2x,%d,%d.\n",i/3,reader->last_byte,count,more_count);
#endif
                  ++i_in;
                }
            }
        }
    }

  return 0;
}

/*
  Read a raster row in the given encoding.

  data_len ... length of remaining image data.
  repeat ... pointer to a repeat counter in which the decompressor
             returns, how many times this raster row has to be repeated.
             This is in fact necessary for compression mode 5.
              
             If a null pointer is passed in, the subroutine assumes, that
             we want to read a raster plane insteadd of a raster row.
             (compression mode 4 and 5 are not applicable for raster planes)

  Returns the number of bytes read or -1 upon errors.
*/
static int pcl_read_raster_row(hpgs_reader *reader,int data_len, int *repeat)
{
  if (!is_pcl_compression_supported(reader->pcl_raster_compression,repeat == NULL))
    // simply ignore unknown compression modes.
    return pcl_ignore_raster_data(reader,data_len);

  hpgs_bool first_row = reader->pcl_raster_data[reader->pcl_raster_plane] == NULL;

  if (first_row)
    {
      // for compression mode 4, read the header first.
      if (reader->pcl_raster_compression == 4 &&
          pcl_read_raster_4_header(reader,data_len))
        return -1;

      if (reader->pcl_raster_src_width <= 0)
        {
          reader->pcl_raster_src_width = (int)
            (reader->pcl_raster_presentation ? reader->x_size : reader->y_size)
            * HP_TO_PT  * reader->pcl_raster_res / 72.0;

          if (reader->verbosity)
            hpgs_log(hpgs_i18n("Estimated PCL src width = %d.\n"),
                     reader->pcl_raster_src_width);

          reader->pcl_raster_mode = 1;

          if (reader->pcl_raster_src_height <= 0)
            {
              reader->pcl_raster_src_height = (int)
                (reader->pcl_raster_presentation ? reader->y_size : reader->x_size)
                * HP_TO_PT  * reader->pcl_raster_res / 72.0;
              
              while (reader->pcl_raster_src_width * reader->pcl_raster_src_height
                     > 1024*16384)
                reader->pcl_raster_src_height /= 2;

              if (reader->verbosity)
                hpgs_log(hpgs_i18n("Estimated PCL src height = %d.\n"),
                         reader->pcl_raster_src_height);
            }
        }

      if (reader->pcl_raster_data_size <= 0)
        {
          reader->pcl_raster_data_size = reader->pcl_raster_src_width;

          if (reader->pcl_palettes[reader->pcl_i_palette]->cid_enc == 3) // direct by pixel
            reader->pcl_raster_data_size *= 3;
          else {
            // Well compression 10 does not support anything else than direct by pixel.
            if (reader->pcl_raster_compression == 10)
              return hpgs_set_error(hpgs_i18n("Incompatible CID encoding %d for compression mode 10."),
                                    reader->pcl_palettes[reader->pcl_i_palette]->cid_enc);
          }            
        }

      reader->pcl_raster_data[reader->pcl_raster_plane] =
        (unsigned char *)malloc(reader->pcl_raster_data_size);
      
      if (!reader->pcl_raster_data[reader->pcl_raster_plane])
        return hpgs_set_error(hpgs_i18n("Out of memory allocating %d bytes of raster bata buffer."),reader->pcl_raster_data_size);

      memset(reader->pcl_raster_data[reader->pcl_raster_plane],
             0,reader->pcl_raster_data_size);
    }

  int n = data_len;

  switch (reader->pcl_raster_compression)
    {
    case 0:
      if (pcl_read_raster_0(reader,data_len))
        return -1;
      break;
          
    case 1:
      if (pcl_read_raster_1(reader,data_len))
        return -1;
      break;
          
    case 2:
      if (pcl_read_raster_2(reader,data_len))
        return -1;
      break;

    case 3:
      if (pcl_read_raster_3(reader,data_len))
        return -1;
      break;

    case 4:
      n = pcl_read_raster_4(reader,first_row ? (data_len-4) : data_len);

      if (n<0)
        return -1;

      // These four bytes have been read during header interpretation
      if (first_row) n+=4;
      break;

    case 5:
      n = pcl_read_raster_5(reader,data_len,repeat);
      break;

    case 9:
      if (pcl_read_raster_9(reader,data_len))
        return -1;
      break;

    case 10:
      if (pcl_read_raster_10(reader,data_len))
        return -1;
      break;

    default:
      return -1;
    }

  return n;
}

static int pcl_fill_scanline(hpgs_reader *reader)
{
  int i;
  hpgs_paint_color c;

  if (!reader->pcl_image)
    {
      if (reader->pcl_raster_src_width <= 0)
        return hpgs_set_error(hpgs_i18n("No PCL source raster width specified."));

      if (reader->pcl_raster_src_height <= 0)
        return hpgs_set_error(hpgs_i18n("No PCL source raster height specified."));

      if (reader->verbosity > 1)
        hpgs_log(hpgs_i18n("Creating %dx%d PCL image.\n"),
                 reader->pcl_raster_src_width,
                 reader->pcl_raster_src_height);

      // set up image, if not done yet.
      switch (reader->pcl_palettes[reader->pcl_i_palette]->cid_enc)
        {
        case 0: // indexed by plane.
        case 1: // indexed by pixel.
          reader->pcl_image = (hpgs_image *)
            hpgs_new_png_image(reader->pcl_raster_src_width,
                               reader->pcl_raster_src_height,
                               8,HPGS_TRUE,HPGS_FALSE,HPGS_FALSE);

          // pull in palette
          if (reader->pcl_image &&
              hpgs_image_set_palette(reader->pcl_image,
                                     reader->pcl_palettes[reader->pcl_i_palette]->colors,256))
            return -1;

#ifdef HPGS_PCL_DEBUG
          for (i=0;i<256;++i)
            hpgs_log("pcl_fill_scanline: color %3d = %3d,%3d,%3d.\n",
                     i,
                     reader->pcl_palettes[reader->pcl_i_palette]->colors[i].r,
                     reader->pcl_palettes[reader->pcl_i_palette]->colors[i].g,
                     reader->pcl_palettes[reader->pcl_i_palette]->colors[i].b );
#endif

          break;
          
        case 2: // direct by plane.
          reader->pcl_image = (hpgs_image *)
            hpgs_new_png_image(reader->pcl_raster_src_width,
                               reader->pcl_raster_src_height,
                               8,HPGS_TRUE,HPGS_FALSE,HPGS_FALSE);
          // set palette
          if (reader->pcl_image)
            {
              hpgs_palette_color palette[8];

              for (i=0; i<8; ++i) 
                {
                  palette[i].r = (i&1)*255;
                  palette[i].g = ((i>>1)&1)*255;
                  palette[i].b = ((i>>2)&1)*255;
                }

              hpgs_image_set_palette(reader->pcl_image,palette,8);
            }
          break;
        
        case 3:
          reader->pcl_image = (hpgs_image *)
            hpgs_new_png_image(reader->pcl_raster_src_width,
                               reader->pcl_raster_src_height,
                               24,HPGS_FALSE,HPGS_FALSE,HPGS_FALSE);
          break;

        default:
          return hpgs_set_error(hpgs_i18n("Invalid PCL Pixel Encoding Mode %d."),
                                reader->pcl_palettes[reader->pcl_i_palette]->cid_enc);
        }

      if (!reader->pcl_image)
        return hpgs_set_error(hpgs_i18n("Out of memory creating PCL image."));

#ifdef HPGS_PCL_DEBUG
      hpgs_log("pcl_fill_scanline: image: width,height,palette = %d,%d,%p.\n",
               reader->pcl_image->width,
               reader->pcl_image->height,
               reader->pcl_image->palette );
#endif
    }

  if (reader->pcl_raster_line >= reader->pcl_raster_src_height)
    {
      if (reader->verbosity)
        hpgs_log(hpgs_i18n("PCL raster line %d is out of range (%dx%d).\n"),
                 reader->pcl_raster_line,
                 reader->pcl_raster_src_width,
                 reader->pcl_raster_src_height);

      return 0;
    }

  switch (reader->pcl_palettes[reader->pcl_i_palette]->cid_enc)
    {
    case 0: // indexed by plane.
      {
        int p;
        int np;

        if (!reader->pcl_raster_data[0])
          return hpgs_set_error("No raster data.");

        for (np=1;np<reader->pcl_raster_planes;++np) {
          if (!reader->pcl_raster_data[np]) break;
        }

        for (i=0; i<reader->pcl_raster_src_width;)
          {
            int mask = 0x80;

            int v[8];

            for (p=0;p<np;++p)
              v[p]  = reader->pcl_raster_data[p][i>>3];

            for (; i<reader->pcl_raster_src_width && mask; ++i, mask>>=1)
              {
                int bit;
                c.index = 0;
                
                for (p=0,bit=1;p<np;++p,bit<<=1)
                  if (v[p] & mask) c.index |= bit;

                if (hpgs_image_put_pixel (reader->pcl_image,i,
                                          reader->pcl_raster_line,&c,1.0))
                  return -1;
              }
          }
      }
      break;

    case 1: // indexed by pixel.
      if (!reader->pcl_raster_data[0])
        return hpgs_set_error(hpgs_i18n("No raster data."));

      switch (reader->pcl_palettes[reader->pcl_i_palette]->cid_bpi)
        {
        case 1:
          for (i=0; i<(reader->pcl_raster_src_width+7)/8;++i)
            {
              int bit = 0x80;
              int j;

              for (j=0;j<8 && 8*i+j < reader->pcl_raster_src_width;++j,bit>>=1)
                {
                  c.index = (reader->pcl_raster_data[0][i] & bit) ? 1 : 0;

                  if (hpgs_image_put_pixel (reader->pcl_image,8*i+j,
                                            reader->pcl_raster_line,&c,1.0))
                    return -1;
                }
            }
          break;

        case 2:
          for (i=0; i<(reader->pcl_raster_src_width+3)/4;++i)
            {
              c.index = reader->pcl_raster_data[0][i] >> 6;
              
              if (hpgs_image_put_pixel (reader->pcl_image,4*i,
                                        reader->pcl_raster_line,&c,1.0))
                return -1;

              c.index = (reader->pcl_raster_data[0][i] >> 4) & 0x03;
              
              if (4*i+1 >= reader->pcl_raster_src_width) break;

              if (hpgs_image_put_pixel (reader->pcl_image,4*i+1,
                                        reader->pcl_raster_line,&c,1.0))
                return -1;

              c.index = (reader->pcl_raster_data[0][i] >> 2) & 0x03;
              
              if (4*i+2 >= reader->pcl_raster_src_width) break;

              if (hpgs_image_put_pixel (reader->pcl_image,4*i+2,
                                        reader->pcl_raster_line,&c,1.0))
                return -1;

              c.index = reader->pcl_raster_data[0][i] & 0x03;
              
              if (4*i+3 >= reader->pcl_raster_src_width) break;

              if (hpgs_image_put_pixel (reader->pcl_image,4*i+3,
                                        reader->pcl_raster_line,&c,1.0))
                return -1;
            }
          break;

        case 4:
          for (i=0; i<(reader->pcl_raster_src_width+1)/2;++i)
            {
              c.index = reader->pcl_raster_data[0][i] >> 4;
              
              if (hpgs_image_put_pixel (reader->pcl_image,2*i,
                                        reader->pcl_raster_line,&c,1.0))
                return -1;


              c.index = reader->pcl_raster_data[0][i] & 0x0f;
              
              if (2*i+1 >= reader->pcl_raster_src_width) break;

              if (hpgs_image_put_pixel (reader->pcl_image,2*i+1,
                                        reader->pcl_raster_line,&c,1.0))
                return -1;
            }
          break;

        case 8:
          for (i=0; i<reader->pcl_raster_src_width;++i)
            {
              c.index = reader->pcl_raster_data[0][i];
              
              if (hpgs_image_put_pixel (reader->pcl_image,i,
                                        reader->pcl_raster_line,&c,1.0))
                return -1;
            }
          break;

        default:
          return hpgs_set_error(hpgs_i18n("Invalid bpi %d."),
                                (int)reader->pcl_palettes[reader->pcl_i_palette]->cid_bpi);
        }
      break;
      
    case 2: // direct by plane.
      if (!reader->pcl_raster_data[0] ||
          !reader->pcl_raster_data[1] ||
          !reader->pcl_raster_data[2]   )
        return hpgs_set_error(hpgs_i18n("No raster data."));

      for (i=0; i<reader->pcl_raster_src_width;)
        {
          int mask = 0x80;

          int v0 = reader->pcl_raster_data[0][i>>3];
          int v1 = reader->pcl_raster_data[1][i>>3];
          int v2 = reader->pcl_raster_data[2][i>>3];

          for (; i<reader->pcl_raster_src_width && mask; ++i, mask>>=1)
            {
              c.r = (v0 & mask) ? 255 : 0;
              c.g = (v1 & mask) ? 255 : 0;
              c.b = (v2 & mask) ? 255 : 0;

              if (hpgs_image_define_color (reader->pcl_image,&c))
                return -1;

              if (hpgs_image_put_pixel (reader->pcl_image,i,
                                        reader->pcl_raster_line,&c,1.0))
                return -1;
            }
        }
      break;
      
    case 3:
      if (!reader->pcl_raster_data[0])
        return hpgs_set_error(hpgs_i18n("No raster data."));


      for (i=0; i<reader->pcl_raster_src_width;++i)
        {
          c.r = reader->pcl_raster_data[0][3*i  ];
          c.g = reader->pcl_raster_data[0][3*i+1];
          c.b = reader->pcl_raster_data[0][3*i+2];

          if (hpgs_image_put_pixel (reader->pcl_image,i,
                                    reader->pcl_raster_line,&c,1.0))
            return -1;
        }
      break;

    default:
      return hpgs_set_error(hpgs_i18n("Invalid PCL Pixel Encoding Mode %d."),reader->pcl_palettes[reader->pcl_i_palette]->cid_bpi);
    }

  return 0;
}

static int pcl_put_image(hpgs_reader *reader)
{
  // setup image placement
  double w,h;
  hpgs_point ll,lr,ur;

  double x0;
  double y0;

  if (reader->pcl_raster_line <= 0 ||
      reader->pcl_raster_src_width  <= 0 ||
      reader->pcl_raster_src_height  <= 0  )
    // don't process any further, if we don't have an image size.
    // An image size is needed for correct cursor positioning.
    return 0;

  if (reader->pcl_raster_line < reader->pcl_raster_src_height)
    {
      if (reader->pcl_image &&
          hpgs_image_resize(reader->pcl_image,reader->pcl_raster_src_width,reader->pcl_raster_line))
        return hpgs_set_error(hpgs_i18n("Error downsizing PCL image."));

      reader->pcl_raster_src_height = reader->pcl_raster_line;
    }

#ifdef HPGS_PCL_DEBUG
  hpgs_log("raster_pres,x_size,y_size,rot=%d,%lg,%lg,%d.\n",
           reader->pcl_raster_presentation,reader->x_size,reader->y_size,
           reader->rotation);
#endif

  if (reader->pcl_raster_mode & 1)
    {
      x0 = reader->pcl_point.x;
      y0 = reader->pcl_point.y;
    }
  else
    if (reader->y_size >= reader->x_size)
      {
        x0 = 0.0;
        y0 = reader->pcl_point.y;
      }
    else
      {
        x0 = reader->pcl_point.x;
        y0 = 0.0;
      }

  // PCL coordinates
  //
  // y  (w,0)-------------(w,h)
  // ^    |                 |
  // |    |                 |
  // |    |                 |
  // |    |                 |
  // |  (0,0)-------------(0,h)
  // +-----------> x

#ifdef HPGS_PCL_DEBUG
  hpgs_log("src:w,h=%d,%d.\n",reader->pcl_raster_src_width,reader->pcl_raster_src_height);
#endif
  if (reader->pcl_raster_mode & 2)
    {
      w = 0.1*reader->pcl_raster_dest_width;
      h = 0.1*reader->pcl_raster_dest_height;
    }
  else
    {
#ifdef HPGS_PCL_DEBUG
  hpgs_log("raster_res=%d.\n",reader->pcl_raster_res);
#endif
      w = reader->pcl_raster_src_width * 72.0 / reader->pcl_raster_res;
      h = reader->pcl_raster_src_height * 72.0 / reader->pcl_raster_res;
    }

#ifdef HPGS_PCL_DEBUG
  hpgs_log("w,h=%lg,%lg.\n",w,h);
#endif

  // 2005-06-13:
  // Well, the code below is completely based on my experience
  // with the testsuite, which is available to me.
  // Up to now, I did not find a consistent description of
  // the PCL coordinate setup in HP's docs. If you have any problems
  // with PCL image placement, please send me an e-mail:
  //
  // wolfgang.glas@ev-i.at
  //
  if (reader->pcl_raster_presentation == 0)
    {
      ur.x =  x0 +
        h * reader->pcl_raster_y_offset/(double)reader->pcl_raster_src_height;
      
      ll.y = y0;

      //
      // Some rare testcases seem to indicate, that we might
      // need a sign change here. However, these testcases 
      // might be the result of a bogus HPGL/2 postprocessor.
      //
      // most notable testcase: 70_B1581_TU_04_NA_02-96-317-E01.plt
      if ((reader->y_size >= reader->x_size && reader->rotation <= 90) ||
          (reader->y_size < reader->x_size && reader->rotation > 90)) {
        ur.y = lr.y = y0 - w;
        ll.x = lr.x = ur.x - h;
      }
      else {
        ur.y = lr.y = y0 + w;
        ll.x = lr.x = ur.x + h;
      }
    }
  else
    {
      ur.x = lr.x = x0 + w;
          
      ur.y = y0 -
        h * reader->pcl_raster_y_offset/(double)reader->pcl_raster_src_height;

      ll.x = x0;
      ll.y = lr.y = ur.y - h;
    }

#ifdef HPGS_PCL_DEBUG
  hpgs_log("raster_pres,x_s,y_s=%d,%lg,%lg.\n",
           reader->pcl_raster_presentation,reader->x_size,reader->y_size);

  hpgs_log("ll,lr,ur=(%lg,%lg),(%lg,%lg),(%lg,%lg).\n",
           ll.x,ll.y,lr.x,lr.y,ur.x,ur.y);
#endif

  // save inline images, if specified.
  if (reader->pcl_image && reader->png_dump_filename)
    {
      int l = strlen(reader->png_dump_filename)+16;
      char *fn = hpgs_alloca(l);

      ++reader->png_dump_count;
      snprintf(fn,l,"%s%04d.png",reader->png_dump_filename,reader->png_dump_count);

      if (reader->verbosity)
        hpgs_log(hpgs_i18n("Dumping inline PCL image to file <%s>.\n"),fn);

      int ret = hpgs_image_write(reader->pcl_image,fn);

      if (ret) return -1;
    }

  // check for discarded image data of unknown compression modes,
  // if the device is unable to process null images.
  // (only plotsize devices normally can process null images...)
  if ((hpgs_device_capabilities(reader->device) & HPGS_DEVICE_CAP_NULLIMAGE) ||
      reader->pcl_image)
    {
      hpgs_point ll_p;
      hpgs_point lr_p;
      hpgs_point ur_p;

      hpgs_matrix_xform(&ll_p,&reader->page_matrix,&ll);
      hpgs_matrix_xform(&lr_p,&reader->page_matrix,&lr);
      hpgs_matrix_xform(&ur_p,&reader->page_matrix,&ur);

#ifdef HPGS_PCL_DEBUG
      hpgs_log("ll_p,lr_p,ur_p=(%lg,%lg),(%lg,%lg),(%lg,%lg).\n",
               ll_p.x,ll_p.y,lr_p.x,lr_p.y,ur_p.x,ur_p.y);
#endif

      hpgs_color patcol;

      double fac = 
        reader->pcl_pattern ? (reader->pcl_pattern == 1 ? 0.0 : 0.5) : 1.0;

      patcol.r = (1.0-fac) + fac * (1.0/256.0) * reader->pcl_foreground_color.r;
      patcol.g = (1.0-fac) + fac * (1.0/256.0) * reader->pcl_foreground_color.g;
      patcol.b = (1.0-fac) + fac * (1.0/256.0) * reader->pcl_foreground_color.b;

      if (hpgs_setpatcol(reader->device,&patcol))
        return -1;

      // draw image
      if (hpgs_drawimage(reader->device,reader->pcl_image,&ll_p,&lr_p,&ur_p))
        return -1;

      if (hpgs_setpatcol(reader->device,&reader->pen_colors[reader->current_pen]))
        return -1;
    }

  // set the cursor position.
  reader->pcl_point = ll;

  return 0;
}

static int pcl_do_cmd(hpgs_reader *reader, int cmd, int arg, int arg_sign,
                      hpgs_bool have_arg)
{
  if (reader->verbosity>1)
    hpgs_log(hpgs_i18n("PCL command %c%c%d%c found.\n"),
             (char)(cmd >> 16),(char)(cmd >> 8),arg,(char)(cmd&0xff));

  switch (cmd)
    {
    case MK_PCL_CMD('%',' ','B'):
      if (!have_arg) return -1;
#ifdef HPGS_PCL_DEBUG
      hpgs_log("leave PCL: arg,pcl_point,hpgl_point=%d,(%lg,%lg),(%lg,%lg).\n",
               arg,
               reader->pcl_point.x,
               reader->pcl_point.y,
               reader->current_point.x,
               reader->current_point.y );
#endif
      if (arg & 1)
        {
          hpgs_point tmp;
          hpgs_matrix_xform(&tmp,&reader->page_matrix,&reader->pcl_point);
          if (hpgs_reader_moveto(reader,&tmp)) return -1;
        }

      pcl_clear_raster_data(reader,PCL_RASTER_CLEAR_IMAGE);
      return 1;
      
    case MK_PCL_CMD('%',' ','X'):
      if (!have_arg) return -1;
      pcl_clear_raster_data(reader,PCL_RASTER_CLEAR_IMAGE);
      return 2;

    case MK_PCL_CMD('*','v','N'): // Source Transparency Mode.
      if (!have_arg) return -1;
      reader->src_transparency = (arg == 0);

#ifdef HPGS_PCL_DEBUG
      hpgs_log("src transp. %d,%d.\n",arg,reader->src_transparency);
#endif
      if (hpgs_setrop3(reader->device,reader->rop3,
                       reader->src_transparency,
                       reader->pattern_transparency ))
          return -1;
      break;

    case MK_PCL_CMD('*','v','O'): // Pattern Transparency Mode.
      if (!have_arg) return -1;
      reader->pattern_transparency = (arg == 0);

#ifdef HPGS_PCL_DEBUG
      hpgs_log("pat transp. %d,%d.\n",arg,reader->pattern_transparency);
#endif
      if (hpgs_setrop3(reader->device,reader->rop3,
                       reader->src_transparency,
                       reader->pattern_transparency ))
          return -1;
      break;

    case MK_PCL_CMD('*','v','T'): // Set Current Pattern.
      if (!have_arg) return -1;
      
      {
        if (reader->verbosity && (arg < 0 || arg > 1))
          hpgs_log(hpgs_i18n("Warning: Invalid pattern %d specified setting pattern to 50%% grey.\n"),arg);

        reader->pcl_pattern = arg;
      }
      break;

    case MK_PCL_CMD('*','l','O'): // Logical operation.
      if (!have_arg) return -1;
      reader->rop3 = arg;

      if (hpgs_setrop3(reader->device,reader->rop3,
                       reader->src_transparency,
                       reader->pattern_transparency ))
          return -1;
      break;

    case MK_PCL_CMD('*','p','P'): // Push/Pop palette.
      if (!have_arg) return -1;
      if (arg < 0 || arg > 1)
        return hpgs_set_error(hpgs_i18n("PCL push/pop palette arg %d invalid."),arg);

      if (arg)
        {
          if (hpgs_reader_pop_pcl_palette(reader)) return -1;
        }
      else
        {
          if (hpgs_reader_push_pcl_palette(reader)) return -1;
        }
      break;

    case MK_PCL_CMD('&','u','D'): // PCL unit
      if (!have_arg) return -1;
      reader->pcl_scale = 72.0/(double)arg;
      break;

    case MK_PCL_CMD('&','k','H'): // horizontal motion index
      if (!have_arg) return -1;
      reader->pcl_hmi = (double)arg*72.0/120.0;
      break;

    case MK_PCL_CMD('&','l','C'):// vertical motion index
      if (!have_arg) return -1;
      reader->pcl_vmi = (double)arg*72.0/48.0;
      break;

    case MK_PCL_CMD('&','a','C'): // horiz. position in columns.
      if (!have_arg) return -1;
      if (arg_sign)
        reader->pcl_point.y += arg * reader->pcl_hmi;
      else
        reader->pcl_point.y = reader->frame_y + arg * reader->pcl_hmi;
      break;

    case MK_PCL_CMD('&','a','H'): // horiz. position in decipoints.
      if (!have_arg) return -1;
      if (arg_sign)
        reader->pcl_point.y += arg * 0.1;
      else
        reader->pcl_point.y = reader->frame_y + arg * 0.1;
      break;

    case MK_PCL_CMD('*','p','X'): // horiz. position in PCL units.
      if (!have_arg) return -1;
      if (arg_sign)
        reader->pcl_point.y += arg * reader->pcl_scale;
      else
        reader->pcl_point.y = reader->frame_y + arg * reader->pcl_scale;
      break;

    case MK_PCL_CMD('&','a','R'): // vert. position in columns.
      if (!have_arg) return -1;
      if (arg_sign)
        reader->pcl_point.x += arg * reader->pcl_vmi;
      else
        reader->pcl_point.x = reader->frame_x + arg * reader->pcl_vmi;
      break;

    case MK_PCL_CMD('&','a','V'): // vert. position in decipoints.
      if (!have_arg) return -1;
      if (arg_sign)
        reader->pcl_point.x += arg * 0.1;
      else
        reader->pcl_point.x = reader->frame_x + arg * 0.1;
      break;

    case MK_PCL_CMD('*','p','Y'): // vert. position in PCL units.
      if (!have_arg) return -1;
      if (arg_sign)
        reader->pcl_point.x += arg * reader->pcl_scale;
      else
        reader->pcl_point.x = reader->frame_x + arg * reader->pcl_scale;
      break;

    case MK_PCL_CMD('*','v','W'): // configure image data
      if (!have_arg) return -1;
      if (reader->pcl_raster_data[reader->pcl_raster_plane])
        return hpgs_set_error(hpgs_i18n("PCL CID command after image data."));

      reader->pcl_palettes[reader->pcl_i_palette]->cid_space = hpgs_getc(reader->in);
      if (reader->pcl_palettes[reader->pcl_i_palette]->cid_space == EOF) return -1;
      reader->pcl_palettes[reader->pcl_i_palette]->cid_enc = hpgs_getc(reader->in);
      if (reader->pcl_palettes[reader->pcl_i_palette]->cid_enc == EOF) return -1;
      reader->pcl_palettes[reader->pcl_i_palette]->cid_bpi = hpgs_getc(reader->in);
      if (reader->pcl_palettes[reader->pcl_i_palette]->cid_bpi == EOF) return -1;
      reader->pcl_palettes[reader->pcl_i_palette]->cid_bpc[0] = hpgs_getc(reader->in);
      if (reader->pcl_palettes[reader->pcl_i_palette]->cid_bpc[0] == EOF) return -1;
      reader->pcl_palettes[reader->pcl_i_palette]->cid_bpc[1] = hpgs_getc(reader->in);
      if (reader->pcl_palettes[reader->pcl_i_palette]->cid_bpc[1] == EOF) return -1;
      reader->pcl_palettes[reader->pcl_i_palette]->cid_bpc[2] = hpgs_getc(reader->in);
      if (reader->pcl_palettes[reader->pcl_i_palette]->cid_bpc[2] == EOF) return -1;

      if (arg == 18)
        {
          // basic interpretation of long form of CID command.
          unsigned char ref_data[12];
          // this is the only form of the CID 18 header we are currently recognizig.
          unsigned char impl_ref_data[12] = { 0,255,0,255,0,255,0,0,0,0,0,0 };

          if (hpgs_istream_read(ref_data,1,sizeof(ref_data),reader->in) != sizeof(ref_data)) return -1;
          
          if (memcmp(ref_data,impl_ref_data,sizeof(ref_data)) != 0)
            return hpgs_set_error(hpgs_i18n("PCL CID command with arg==18 and non-trivial colorspace is unimplemented."));
        }
      else if (arg!=6)
        return hpgs_set_error(hpgs_i18n("PCL CID command with arg!=6 and arg !=18 is unimplemented."));

#ifdef HPGS_PCL_DEBUG
      hpgs_log("CID: %d %d %d %d %d %d.\n",
               reader->pcl_palettes[reader->pcl_i_palette]->cid_space,
               reader->pcl_palettes[reader->pcl_i_palette]->cid_enc,
               reader->pcl_palettes[reader->pcl_i_palette]->cid_bpi,
               reader->pcl_palettes[reader->pcl_i_palette]->cid_bpc[0],
               reader->pcl_palettes[reader->pcl_i_palette]->cid_bpc[1],
               reader->pcl_palettes[reader->pcl_i_palette]->cid_bpc[2] );
#endif

      if (reader->pcl_palettes[reader->pcl_i_palette]->cid_enc == 0 &&
          (reader->pcl_palettes[reader->pcl_i_palette]->cid_bpi < 1 || reader->pcl_palettes[reader->pcl_i_palette]->cid_bpi > 8))
        return hpgs_set_error(hpgs_i18n("Invalid bits per index %d for PCL CID encoding 0."),reader->pcl_palettes[reader->pcl_i_palette]->cid_bpi);

      if (reader->pcl_palettes[reader->pcl_i_palette]->cid_enc == 1 &&
          (reader->pcl_palettes[reader->pcl_i_palette]->cid_bpi != 1 &&
           reader->pcl_palettes[reader->pcl_i_palette]->cid_bpi != 2 &&
           reader->pcl_palettes[reader->pcl_i_palette]->cid_bpi != 4 &&
           reader->pcl_palettes[reader->pcl_i_palette]->cid_bpi != 8   ))
        return hpgs_set_error(hpgs_i18n("Inavlid bits per index %d for PCL CID encoding 1."),reader->pcl_palettes[reader->pcl_i_palette]->cid_bpi);

      if (reader->pcl_palettes[reader->pcl_i_palette]->cid_enc == 2)
        reader->pcl_raster_planes = 3;
      else if (reader->pcl_palettes[reader->pcl_i_palette]->cid_enc == 0)
        reader->pcl_raster_planes = reader->pcl_palettes[reader->pcl_i_palette]->cid_bpi;
      else
        reader->pcl_raster_planes = 1;

      break;

    case MK_PCL_CMD('*','v','A'): // color component #1
      if (!have_arg) return -1;
      reader->pcl_palettes[reader->pcl_i_palette]->last_color.r = arg;
      break;

    case MK_PCL_CMD('*','v','B'): // color component #2
      if (!have_arg) return -1;
      reader->pcl_palettes[reader->pcl_i_palette]->last_color.g = arg;
      break;

    case MK_PCL_CMD('*','v','C'): // color component #3
      if (!have_arg) return -1;
      reader->pcl_palettes[reader->pcl_i_palette]->last_color.b = arg;
      break;

    case MK_PCL_CMD('*','v','I'): // assign color index
      if (!have_arg) return -1;
      if (arg < 0 || arg >= 256)
        return hpgs_set_error(hpgs_i18n("PCL Color index %d is out of bounds."),arg);
        
      reader->pcl_palettes[reader->pcl_i_palette]->colors[arg] = reader->pcl_palettes[reader->pcl_i_palette]->last_color;

      reader->pcl_palettes[reader->pcl_i_palette]->last_color.r = 0;
      reader->pcl_palettes[reader->pcl_i_palette]->last_color.g = 0;
      reader->pcl_palettes[reader->pcl_i_palette]->last_color.b = 0;
      break;

    case MK_PCL_CMD('*','r','A'): // start raster graphics
      if (!have_arg) return -1;
      //      if (reader->pcl_raster_mode >= 0)
      //  return hpgs_set_error(hpgs_i18n("Nested PCL start raster graphics commands."));

      reader->pcl_raster_mode = arg;
      break;

    case MK_PCL_CMD('*','r','F'): // Raster Presentation Mode
      if (!have_arg) return -1;
      if (arg != 0 && arg != 3)
        return hpgs_set_error(hpgs_i18n("PCL Raster Presentation Mode %d is invalid."),arg);

      reader->pcl_raster_presentation = arg;
      break;

    case MK_PCL_CMD('*','t','R'): // Raster Resolution
      if (!have_arg) return -1;
      if (arg < 75 || arg > 2400)
        return hpgs_set_error(hpgs_i18n("PCL Raster Resolution %d is out of bounds."),arg);

      reader->pcl_raster_res = arg;
      break;

    case MK_PCL_CMD('*','r','S'): // source Raster Width
      if (!have_arg) return -1;

      if (reader->pcl_raster_data[0])
        return hpgs_set_error(hpgs_i18n("PCL Source raster width command after image data."));

      // if (reader->pcl_raster_src_width > 0)
      //   return hpgs_set_error("Repeated occurrence of PCL Source raster width.");

      if (arg < 0 || arg > 32767)
        return hpgs_set_error(hpgs_i18n("PCL Source raster width %d is out of bounds."),arg);

      reader->pcl_raster_src_width = arg;
      break;

    case MK_PCL_CMD('*','r','T'): // source Raster Height
      if (!have_arg) return -1;

      if (reader->pcl_raster_data[0])
        return hpgs_set_error(hpgs_i18n("PCL Source raster height command after image data."));

      // if (reader->pcl_raster_src_height > 0)
      //   return hpgs_set_error("Repeated occurrence of PCL Source raster height.");

      if (arg < 0 || arg > 32767)
        return hpgs_set_error(hpgs_i18n("PCL Source raster height %d is out of bounds."),arg);

      reader->pcl_raster_src_height = arg;
      break;

    case MK_PCL_CMD('*','t','H'): // destination Raster Width
      if (!have_arg) return -1;

      if (arg > 65535) {

        if (reader->verbosity > 0)
          hpgs_log(hpgs_i18n("Warning: PCL Destination raster width %d is out of range and will be clamped to 65535.\n"),arg);
        
        arg = 65535;
      }

      if (arg < 0)
        return hpgs_set_error(hpgs_i18n("PCL Destination raster width %d is out of bounds."),arg);

      reader->pcl_raster_dest_width = arg;
      break;

    case MK_PCL_CMD('*','t','V'): // destination Raster Height
      if (!have_arg) return -1;

      if (arg > 65535) {

        if (reader->verbosity > 0)
          hpgs_log(hpgs_i18n("Warning: PCL Destination raster height %d is out of range and will be clamped to 65535.\n"),arg);
        
        arg = 65535;
      }

      if (arg < 0)
        return hpgs_set_error(hpgs_i18n("PCL Destination raster height %d is out of bounds."),arg);

      reader->pcl_raster_dest_height = arg;
      break;

    case MK_PCL_CMD('*','b','Y'): // raster Y offset
      if (!have_arg) return -1;
      reader->pcl_raster_y_offset = arg;
      break;

    case MK_PCL_CMD('*','b','M'): // Raster Compression Mode
      if (!have_arg) return -1;

      if (!is_pcl_compression_supported(arg,HPGS_FALSE) && reader->verbosity > 0)
        hpgs_log(hpgs_i18n("Warning: Ignoring image data in PCL Raster Compression Mode %d.\n"),arg);

      reader->pcl_raster_compression = arg;
      break;

    case MK_PCL_CMD('*','b','V'): // Transfer Raster Data by Plane
      if (!have_arg) return -1;
      if (arg < 0)
        return hpgs_set_error(hpgs_i18n("PCL Raster Row length %d is invalid."),arg);

      if (pcl_read_raster_row(reader,arg,NULL) < 0) return -1; 
      reader->pcl_raster_plane = (reader->pcl_raster_plane+1)%reader->pcl_raster_planes;
      break;

    case MK_PCL_CMD('*','b','W'): // Transfer Raster Data by Row
      if (!have_arg) return -1;
      if (arg < 0)
        return hpgs_set_error(hpgs_i18n("PCL Raster Row length %d is invalid."),arg);

      // Raster compression mode 4 adjusts the image width and bytes per
      // raster line, so such a block must be the first in line.
      if (reader->pcl_raster_compression == 4 &&
          reader->pcl_raster_data[reader->pcl_raster_plane])
        return hpgs_set_error(hpgs_i18n("Raster compression mode 4 data block "
                                        "among other raster data is unsupported."));

      if ((reader->pcl_raster_compression == 4 ||
           reader->pcl_raster_compression == 5   ) && 
          reader->pcl_raster_src_height < 128         )
        reader->pcl_raster_src_height = 128;
      
      do {

        int repeat = 1;
        int n = pcl_read_raster_row(reader,arg,&repeat);

#ifdef HPGS_PCL_DEBUG_BLOCK_MODES
      hpgs_log("pcl_read_raster_row: compression,arg,n,repeat=%d,%d,%d,%d.\n",
               reader->pcl_raster_compression,arg,n,repeat);
#endif

        if (n<0) return -1; 
        reader->pcl_raster_plane = 0;

        while (repeat > 0)
          {
            // For the sake of performance, only process image data
            // for real drawing devices, not for the plotsize device. 
            // Additionally, ignore unknown raster compression modes.
            if ((hpgs_device_capabilities(reader->device) & HPGS_DEVICE_CAP_PLOTSIZE) == 0 &&
                is_pcl_compression_supported(reader->pcl_raster_compression,HPGS_FALSE) &&
                reader->pcl_raster_src_width > 0 &&
                pcl_fill_scanline(reader))
              return -1; 
            
            ++reader->pcl_raster_line;

            // emit partial images.
            if (reader->pcl_raster_src_height > 1 &&
                reader->pcl_raster_line >= reader->pcl_raster_src_height)
              {
                if (pcl_put_image(reader)) return -1; 
                pcl_clear_raster_data(reader,PCL_RASTER_CLEAR_PARTIAL);
                
              }

            --repeat;
          }
        
        arg -= n;
        
      } while ((reader->pcl_raster_compression == 4 ||
                reader->pcl_raster_compression == 5   ) &&
               arg > 0);

      if (reader->pcl_raster_compression == 4 ||
          reader->pcl_raster_compression == 5   )
        {
          if (pcl_put_image(reader)) return -1; 
          pcl_clear_raster_data(reader,PCL_RASTER_CLEAR_IMAGE);
        }

      break;

    case MK_PCL_CMD('*','r','B'): // End Raster Graphics
    case MK_PCL_CMD('*','r','C'): // End Raster Graphics
        if (have_arg) return -1;
        if (pcl_put_image(reader)) return -1; 
        pcl_clear_raster_data(reader,PCL_RASTER_CLEAR_IMAGE);
        break;

    case MK_PCL_CMD('*','v','S'): // Foreground Color
        if (!have_arg) return -1;
        
        if (arg >= 0 && arg < 256)
          reader->pcl_foreground_color = 
            reader->pcl_palettes[reader->pcl_i_palette]->colors[arg];

        break;

    default:
      if (reader->verbosity)
        hpgs_log(hpgs_i18n("Unknown PCL command %c%c%d%c found.\n"),
                 (char)(cmd >> 16),(char)(cmd >> 8),arg,(char)(cmd&0xff));
    }
#ifdef HPGS_PCL_DEBUG
  if ((cmd >> 16) == '&' || cmd == MK_PCL_CMD('*','p','X') || cmd == MK_PCL_CMD('*','p','Y'))
    hpgs_log("%c%c%d%c: pcl_point=(%lg,%lg).\n",
             (char)(cmd >> 16),(char)(cmd >> 8),arg,(char)(cmd&0xff),
             reader->pcl_point.x,
             reader->pcl_point.y);
#endif

  return 0;
}

/*!
  Read a PCL block of the file.

  Return values:
   \li -1 Error, unexpected EOF.
   \li 0 Success, return to HPGL context.
   \li 1 Success, return to PJL context.
   \li 2 Success, EOF reached.
*/
int hpgs_reader_do_PCL(hpgs_reader *reader, hpgs_bool take_pos)
{
  int cmd1,cmd2;
  reader->bytes_ignored = 0;
  reader->eoc = 0;
  int ret = -1;

#ifdef HPGS_PCL_DEBUG
      hpgs_log("enter PCL: arg,pcl_point,hpgl_point=%d,(%lg,%lg),(%lg,%lg).\n",
               take_pos,
               reader->pcl_point.x,
               reader->pcl_point.y,
               reader->current_point.x,
               reader->current_point.y );
#endif

  if (take_pos)
    {
      reader->pcl_point.x = reader->current_point.x;
      reader->pcl_point.y = reader->current_point.y;
      hpgs_matrix_ixform(&reader->pcl_point,&reader->pcl_point,&reader->page_matrix);
    }

  while ((reader->last_byte = hpgs_getc(reader->in)) != EOF)
    {
      if (reader->interrupted) goto interrupted_escape;

      if (reader->last_byte != HPGS_ESC) continue;

      reader->last_byte = hpgs_getc(reader->in);

      if (reader->last_byte == EOF) break;

      cmd1 = reader->last_byte;
      cmd2 = ' ';

      switch (cmd1)
	{
	case 'E':
          hpgs_reader_set_default_state (reader);
	  break;
	case '&':
	case '*':
         reader->last_byte = hpgs_getc(reader->in);
         if (reader->last_byte == EOF) goto unexpected_eof;
         // invalid group character ?
         if (reader->last_byte < 96 || reader->last_byte >126) break;
         cmd2 = reader->last_byte;
	case '%':
          {
            hpgs_bool do_continue;
            do
              {
                int cmd = 0;
                int arg,arg_sign;
                int have_arg = hpgs_reader_read_pcl_int(reader,&arg,&arg_sign);

                // check for an ESC charcter here
                // (which has already put back by ungetc() in pcl_read_int)
                // and continue with command parsing from the beginning.
                // This fixes some broken PCL files.
                if (reader->last_byte == HPGS_ESC) break;

                do_continue = HPGS_FALSE;

                if (have_arg < 0)
                  goto unexpected_eof;
                
                reader->last_byte = hpgs_getc(reader->in);

                if (reader->last_byte == EOF) goto unexpected_eof;

                if (reader->last_byte >= 'a' && reader->last_byte <= 'z')
                  {
                    cmd = MK_PCL_CMD(cmd1,cmd2,reader->last_byte + ('A' - 'a'));
                    do_continue = HPGS_TRUE;
                  }
                else if (reader->last_byte >= 'A' && reader->last_byte <= 'Z')
                  cmd = MK_PCL_CMD(cmd1,cmd2,reader->last_byte);

                if (cmd)
                  switch (pcl_do_cmd(reader,cmd,arg,arg_sign,have_arg))
                    {
                    case 1:
                      return 0;
                    case 2:
                      return 1;
                    case 0:
                      break;
                    default:
                      goto unexpected_eof;
                    }
              }
            while (do_continue);
          }
	  break;
	}
    }

  ret = 2;

 unexpected_eof:

  if (reader->pcl_image)
    ret = hpgs_error_ctxt("Error in PCL image");

 interrupted_escape:
  pcl_clear_raster_data(reader,PCL_RASTER_CLEAR_FORCE);
  return ret;
}
