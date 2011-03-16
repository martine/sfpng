#include "sfpng.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

static void dump_attrs(sfpng_decoder* decoder) {
  printf("dimensions: %dx%d\n",
         sfpng_decoder_get_width(decoder),
         sfpng_decoder_get_height(decoder));
  printf("bit depth: %d  color type: %d\n",
         sfpng_decoder_get_depth(decoder),
         sfpng_decoder_get_color_type(decoder));
  int interlaced = sfpng_decoder_get_interlaced(decoder);
  printf("interlaced: %s\n", interlaced ? "yes" : "no");

  if (sfpng_decoder_has_gamma(decoder))
    printf("gamma: %.2f\n", sfpng_decoder_get_gamma(decoder));

  const uint8_t* palette = sfpng_decoder_get_palette(decoder);
  if (palette) {
    printf("palette:");
    int entries = sfpng_decoder_get_palette_entries(decoder);
    int i;
    for (i = 0; i < entries * 3; i += 3) {
      printf(" %02x%02x%02x",
             palette[i],
             palette[i+1],
             palette[i+2]);
    }
    printf("\n");
  }
}

static void row_func(void* context,
                     sfpng_decoder* decoder,
                     int row,
                     const void* buf,
                     size_t bytes) {
  if (row == 0)
    dump_attrs(decoder);

  if (sfpng_decoder_get_interlaced(decoder))
    return;

  const uint8_t* buf_bytes = buf;
  printf("%3d:", row);
  int i;
  for (i = 0; i < bytes; ++i)
    printf("%02x", buf_bytes[i]);
  printf("\n");
}

static void unknown_chunk(void* context, sfpng_decoder* decoder,
                          char chunk_type[4],
                          const void* buf, size_t bytes) {
  if (0)
  printf("unknown chunk: %c%c%c%c, length %d\n",
         chunk_type[0], chunk_type[1], chunk_type[2], chunk_type[3],
         (int)bytes);
}

int main(int argc, char* argv[]) {
  const char* filename = argv[1];
  if (!filename) {
    fprintf(stderr, "usage: %s pngfile\n", argv[0]);
    return 1;
  }

  FILE* f = fopen(filename, "rb");
  if (!f) {
    perror("fopen");
    return 1;
  }

  sfpng_decoder* decoder = sfpng_decoder_new();
  sfpng_decoder_set_row_func(decoder, row_func);
  sfpng_decoder_set_unknown_chunk_func(decoder, unknown_chunk);

  char buf[4096];
  size_t len;
  while ((len = fread(buf, 1, sizeof(buf), f)) >= 0) {
    sfpng_status status = sfpng_decoder_write(decoder, buf, len);
    if (status != SFPNG_SUCCESS) {
      if (status == SFPNG_ERROR_ALLOC_FAILED)
        printf("alloc failed\n");
      else
        printf("invalid image\n");
      return 1;
    }
    if (len == 0)
      break;
  }
  sfpng_decoder_free(decoder);

  return 0;
}
