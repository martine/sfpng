#include "sfpng.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>
#include <stdint.h>

#include <zlib.h>

#include "crc.h"

#define min(a, b) ((a) < (b) ? (a) : (b))

typedef enum {
  STATE_SIGNATURE,
  STATE_CHUNK_HEADER,
  STATE_CHUNK_DATA,
  STATE_CHUNK_CRC,
} decode_state;

typedef enum {
  COLOR_GRAYSCALE = 0,
  COLOR_TRUECOLOR = 2,
  COLOR_INDEXED = 3,
  COLOR_GRAYSCALE_ALPHA = 4,
  COLOR_TRUECOLOR_ALPHA = 6,
} color_type;

struct _sfpng_decoder {
  crc_table crc_table;

  sfpng_row_func row_func;

  /* Header decoding state. */
  decode_state state;
  uint8_t in_buf[8];
  int in_len;

  /* Chunk decoding state. */
  int chunk_len;
  char chunk_type[4];
  int chunk_ofs;
  uint8_t* chunk_buf;

  /* Image properties, read from IHDR chunk. */
  uint32_t width;
  uint32_t height;
  int bit_depth;
  color_type color_type;

  /* Derived image properties, computed from above. */
  int stride;
  int bytes_per_pixel;

  /* IDAT decoding state. */
  z_stream zlib_stream;
  uint8_t* scanline_buf;
  uint8_t* scanline_prev_buf;
  int scanline_row;
};

typedef struct {
  const uint8_t* buf;
  int len;
} stream;

static void stream_consume(stream* stream, int bytes) {
  stream->buf += bytes;
  stream->len -= bytes;
}
static uint32_t stream_read_uint32(stream* stream) {
  uint32_t i;
  memcpy(&i, stream->buf, 4);
  stream_consume(stream, 4);
  return ntohl(i);
}
static uint8_t stream_read_byte(stream* stream) {
  uint8_t i = stream->buf[0];
  stream_consume(stream, 1);
  return i;
}

static void fill_buffer(uint8_t* buf, int* have_len, int want_len,
                        stream* src) {
  int want_bytes = want_len - *have_len;
  int take_bytes = min(src->len, want_bytes);

  memcpy(buf + *have_len, src->buf, take_bytes);
  *have_len += take_bytes;
  stream_consume(src, take_bytes);
}

#define PNG_TAG(a,b,c,d) ((uint32_t)((a<<24)|(b<<16)|(c<<8)|d))

static const char png_signature[8] = {
  137, 80, 78, 71, 13, 10, 26, 10
};

sfpng_decoder* sfpng_decoder_new() {
  sfpng_decoder* decoder;

  decoder = malloc(sizeof(*decoder));
  if (!decoder)
    return NULL;

  memset(decoder, 0, sizeof(*decoder));
  crc_init_table(decoder->crc_table);

  return decoder;
}

enum filter_type {
  FILTER_NONE = 0,
  FILTER_SUB,
  FILTER_UP,
  FILTER_AVERAGE,
  FILTER_PAETH
};

static sfpng_status reconstruct_filter(sfpng_decoder* decoder) {
  /* 9.2 Filter types for filter method 0 */
  uint8_t* buf = decoder->scanline_buf;
  int filter_type = buf[0];
  ++buf;

  switch (filter_type) {
  case FILTER_NONE:
    break;
  case FILTER_SUB: {
    int i;
    for (i = decoder->bytes_per_pixel; i < decoder->stride; ++i) {
      uint8_t a = buf[i - decoder->bytes_per_pixel];
      buf[i] = buf[i] + a;
    }
    break;
  }
  case FILTER_UP: {
    int i;
    for (i = 0; i < decoder->stride; ++i) {
      uint8_t b = decoder->scanline_prev_buf[i + 1];
      buf[i] = buf[i] + b;
    }
    break;
  }
  case FILTER_AVERAGE: {
    int i;
    for (i = 0; i < decoder->stride; ++i) {
      int prev = i - decoder->bytes_per_pixel;
      uint8_t a = prev >= 0 ? buf[prev] : 0;
      uint8_t b = decoder->scanline_prev_buf[i + 1];
      int avg = (a + b) / 2;
      buf[i] = buf[i] + avg;
    }
    break;
  }
  case FILTER_PAETH: {
    int i;
    for (i = 0; i < decoder->stride; ++i) {
      int prev = i - decoder->bytes_per_pixel;
      int a = prev >= 0 ? buf[prev] : 0;
      int b = decoder->scanline_prev_buf[i + 1];
      int c = prev >= 0 ? decoder->scanline_prev_buf[prev + 1] : 0;

      int p = a + b - c;
      int pa = abs(p - a);
      int pb = abs(p - b);
      int pc = abs(p - c);
      uint8_t old = buf[i];
      if (pa <= pb && pa <= pc)
        buf[i] = buf[i] + a;
      else if (pb <= pc)
        buf[i] = buf[i] + b;
      else
        buf[i] = buf[i] + c;
    }
    break;
  }
  default:
    return SFPNG_ERROR_BAD_FILTER;
  }
  return SFPNG_SUCCESS;
}

static sfpng_status parse_color(sfpng_decoder* decoder,
                                int bit_depth,
                                int color_type) {
  switch (color_type) {
  case COLOR_GRAYSCALE: {
    if (bit_depth != 1 && bit_depth != 2 && bit_depth != 4 &&
        bit_depth != 8 && bit_depth != 16) {
      return SFPNG_ERROR_BAD_ATTRIBUTE;
    }
    break;
  }
  case COLOR_TRUECOLOR: {
    if (bit_depth != 8 && bit_depth != 16)
      return SFPNG_ERROR_BAD_ATTRIBUTE;
    break;
  }
  case COLOR_INDEXED: {
    if (bit_depth != 1 && bit_depth != 2 && bit_depth != 4 && bit_depth != 8)
      return SFPNG_ERROR_BAD_ATTRIBUTE;
    break;
  }
  case COLOR_GRAYSCALE_ALPHA: {
    if (bit_depth != 8 && bit_depth != 16)
      return SFPNG_ERROR_BAD_ATTRIBUTE;
    break;
  }
  case COLOR_TRUECOLOR_ALPHA: {
    if (bit_depth != 8 && bit_depth != 16)
      return SFPNG_ERROR_BAD_ATTRIBUTE;
    break;
  }
  default:
    return SFPNG_ERROR_BAD_ATTRIBUTE;
  }

  /* If we get here, the bit depth / color type combination has been
     verified. */
  decoder->bit_depth = bit_depth;
  decoder->color_type = color_type;
  return SFPNG_SUCCESS;
}

static sfpng_status process_chunk(sfpng_decoder* decoder) {
  uint32_t type = PNG_TAG(decoder->chunk_type[0],
                          decoder->chunk_type[1],
                          decoder->chunk_type[2],
                          decoder->chunk_type[3]);
  stream src = { decoder->chunk_buf, decoder->chunk_len };

  switch (type) {
  case PNG_TAG('I','H','D','R'): {
    /* 11.2.2 IHDR Image header */
    if (decoder->chunk_len != 13)
      return SFPNG_ERROR_BAD_ATTRIBUTE;

    decoder->width = stream_read_uint32(&src);
    decoder->height = stream_read_uint32(&src);
    if (decoder->width == 0 ||
        decoder->height == 0) {
      return SFPNG_ERROR_BAD_ATTRIBUTE;
    }

    int bit_depth = stream_read_byte(&src);
    int color_type = stream_read_byte(&src);
    sfpng_status status = parse_color(decoder, bit_depth, color_type);
    if (status != SFPNG_SUCCESS)
      return status;

    /* Compression/filter are currently unused. */
    int compression = stream_read_byte(&src);
    if (compression != 0)
      return SFPNG_ERROR_BAD_ATTRIBUTE;
    int filter = stream_read_byte(&src);
    if (filter != 0)
      return SFPNG_ERROR_BAD_ATTRIBUTE;

    int interlace = stream_read_byte(&src);
    if (interlace != 0 && interlace != 1)
      return SFPNG_ERROR_BAD_ATTRIBUTE;

    /* Done parsing.  Now compute derived attributes. */
    int scanline_bits;
    switch (decoder->color_type) {
    case COLOR_GRAYSCALE:
    case COLOR_INDEXED:
      scanline_bits = decoder->width * decoder->bit_depth;
      decoder->bytes_per_pixel =
          decoder->bit_depth < 8 ? 1 : (decoder->bit_depth / 8);
      break;
    case COLOR_TRUECOLOR:
      scanline_bits = decoder->width * decoder->bit_depth * 3;
      decoder->bytes_per_pixel = 3 * (decoder->bit_depth / 8);
      break;
    case COLOR_GRAYSCALE_ALPHA:
      scanline_bits = decoder->width * decoder->bit_depth * 2;
      decoder->bytes_per_pixel = 2 * (decoder->bit_depth / 8);
      break;
    case COLOR_TRUECOLOR_ALPHA:
      scanline_bits = decoder->width * decoder->bit_depth * 4;
      decoder->bytes_per_pixel = 4 * (decoder->bit_depth / 8);
      break;
    }
    /* Round scanline_bits up to the nearest byte. */
    decoder->stride = ((scanline_bits + 7) / 8);

    /* Allocate scanline buffers, with extra byte for filter tag. */
    decoder->scanline_buf = malloc(1 + decoder->stride);
    if (!decoder->scanline_buf)
      return SFPNG_ERROR_ALLOC_FAILED;
    decoder->scanline_prev_buf = malloc(1 + decoder->stride);
    if (!decoder->scanline_prev_buf)
      return SFPNG_ERROR_ALLOC_FAILED;
    memset(decoder->scanline_prev_buf, 0, 1 + decoder->stride);

    break;
  }
  case PNG_TAG('p','H','Y','s'): {
    /* 11.3.5.3 pHYs Physical pixel dimensions */
    if (decoder->chunk_len != 9)
      return SFPNG_ERROR_BAD_ATTRIBUTE;
    /* Don't care. */
    break;
  }
  case PNG_TAG('I','D','A','T'): {
    /* image data */
    int needs_init = !decoder->zlib_stream.next_in;

    decoder->zlib_stream.next_in = (uint8_t*)src.buf;
    decoder->zlib_stream.avail_in = src.len;
    if (needs_init) {
      if (inflateInit(&decoder->zlib_stream) != Z_OK)
        return SFPNG_ERROR_ZLIB_ERROR;

      decoder->zlib_stream.next_out = decoder->scanline_buf;
      decoder->zlib_stream.avail_out = 1 + decoder->stride;
    }

    while (decoder->zlib_stream.avail_in) {
      int status = inflate(&decoder->zlib_stream, Z_SYNC_FLUSH);
      if (status != Z_OK && status != Z_STREAM_END)
        return SFPNG_ERROR_ZLIB_ERROR;
      if (decoder->zlib_stream.avail_out == 0) {
        /* Decoded line. */
        sfpng_status status = reconstruct_filter(decoder);
        if (status != SFPNG_SUCCESS)
          return status;
        if (decoder->row_func) {
          decoder->row_func(/* XXX */ NULL, decoder, decoder->scanline_row,
                            decoder->scanline_buf + 1, decoder->stride);
        }
        ++decoder->scanline_row;

        /* Swap buffers, so prev points at the row we just finished. */
        uint8_t* tmp = decoder->scanline_prev_buf;
        decoder->scanline_prev_buf = decoder->scanline_buf;
        decoder->scanline_buf = tmp;

        /* Set up for next line. */
        decoder->zlib_stream.next_out = decoder->scanline_buf;
        decoder->zlib_stream.avail_out = 1 + decoder->stride;
      }
    }

    break;
  }
  case PNG_TAG('I', 'E', 'N', 'D'): {
    /* 11.2.5 IEND Image trailer */
    if (decoder->chunk_len != 0)
      return SFPNG_ERROR_BAD_ATTRIBUTE;
    if (decoder->zlib_stream.next_in) {
      int status = inflateEnd(&decoder->zlib_stream);
      decoder->zlib_stream.next_in = NULL;
      if (status != Z_OK)
        return SFPNG_ERROR_ZLIB_ERROR;
    }
    break;
  }
  default:
    printf("WARN: unhandled tag %c%c%c%c\n",
           decoder->chunk_type[0],
           decoder->chunk_type[1],
           decoder->chunk_type[2],
           decoder->chunk_type[3]);
  }
  return SFPNG_SUCCESS;
}

void sfpng_decoder_set_row_func(sfpng_decoder* decoder,
                                sfpng_row_func row_func) {
  decoder->row_func = row_func;
}
int sfpng_decoder_get_width(sfpng_decoder* decoder) {
  return decoder->width;
}
int sfpng_decoder_get_height(sfpng_decoder* decoder) {
  return decoder->height;
}

sfpng_status sfpng_decoder_write(sfpng_decoder* decoder,
                                 const void* buf,
                                 size_t bytes) {
  stream src = { buf, bytes };

  while (src.len > 0) {
    switch (decoder->state) {
    case STATE_SIGNATURE: {
      /* Want 8 bytes of signature in buffer. */
      fill_buffer(decoder->in_buf, &decoder->in_len, 8, &src);
      if (decoder->in_len < 8)
        return SFPNG_SUCCESS;

      if (memcmp(decoder->in_buf, png_signature, 8) != 0)
        return SFPNG_ERROR_BAD_SIGNATURE;

      decoder->state = STATE_CHUNK_HEADER;
      decoder->in_len = 0;
      /* Fall through. */
    }
    case STATE_CHUNK_HEADER: {
      /* Want 8 bytes of chunk header. */
      fill_buffer(decoder->in_buf, &decoder->in_len, 8, &src);
      if (decoder->in_len < 8)
        return SFPNG_SUCCESS;

      memcpy(&decoder->chunk_len, decoder->in_buf, 4);
      decoder->chunk_len = ntohl(decoder->chunk_len);
      memcpy(&decoder->chunk_type, decoder->in_buf + 4, 4);

      if (decoder->chunk_len) {
        decoder->chunk_buf = realloc(decoder->chunk_buf, decoder->chunk_len);
        if (!decoder->chunk_buf)
          return SFPNG_ERROR_ALLOC_FAILED;
      }

      decoder->state = STATE_CHUNK_DATA;
      decoder->chunk_ofs = 0;
      /* Fall through. */
    }
    case STATE_CHUNK_DATA: {
      fill_buffer(decoder->chunk_buf, &decoder->chunk_ofs, decoder->chunk_len,
                  &src);
      if (decoder->chunk_ofs < decoder->chunk_len)
        return SFPNG_SUCCESS;

      decoder->state = STATE_CHUNK_CRC;
      decoder->in_len = 0;
      /* Fall through. */
    }
    case STATE_CHUNK_CRC: {
      fill_buffer(decoder->in_buf, &decoder->in_len, 4, &src);
      if (decoder->in_len < 4)
        return SFPNG_SUCCESS;

      uint32_t expected_crc;
      memcpy(&expected_crc, decoder->in_buf, 4);
      expected_crc = ntohl(expected_crc);

      uint32_t actual_crc =
          crc_compute(decoder->crc_table, decoder->chunk_type,
                      decoder->chunk_buf, decoder->chunk_len);

      if (actual_crc != expected_crc)
        return SFPNG_ERROR_BAD_CRC;

      sfpng_status status = process_chunk(decoder);
      if (status != SFPNG_SUCCESS)
        return status;

      decoder->state = STATE_CHUNK_HEADER;
      decoder->in_len = 0;
      break;
    }
    }
  }

  return SFPNG_SUCCESS;
}

void sfpng_decoder_free(sfpng_decoder* decoder) {
  if (decoder->chunk_buf)
    free(decoder->chunk_buf);
  if (decoder->scanline_buf)
    free(decoder->scanline_buf);
  if (decoder->scanline_prev_buf)
    free(decoder->scanline_prev_buf);
  if (decoder->zlib_stream.next_in) {
    int status = inflateEnd(&decoder->zlib_stream);
    /* We don't care about a bad status at this point. */
  }
  free(decoder);
}
