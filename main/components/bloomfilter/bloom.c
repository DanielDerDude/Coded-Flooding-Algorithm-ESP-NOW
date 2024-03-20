/*
 *  Copyright (c) 2012-2022, Jyri J. Virkki
 *  All rights reserved.
 *
 *  This file is under BSD license. See LICENSE file.
 */

/*
 * Refer to bloom.h for documentation on the public interfaces.
 */

#ifndef _INCLUDES_H_
#define _INCLUDES_H_
#include "../../includes.h"
#endif

#include "bloom.h"
#include "murmurhash2.h"

#define TAG_BLOOM "bloom"

inline static int test_bit_set_bit(uint8_t * buf, uint64_t bit, int8_t set_bit)
{
  uint64_t byte = bit >> 3;
  uint8_t c = buf[byte];             // expensive memory access
  uint8_t mask = 1 << (bit % 8ul);

  if (c & mask) {
    return 1;
  } else {
    if (set_bit) {
      buf[byte] = c | mask;
    }
    return 0;
  }
}

static int8_t bloom_check_add(struct bloom * bloom, const void * buffer, uint32_t len, int8_t add)
{
  if (bloom->ready == 0) {
    ESP_LOGE(TAG_BLOOM, "bloom at %p not initialized!", (void *)bloom);
    return -1;
  }

  uint8_t hits = 0;
  uint32_t a = murmurhash2(buffer, len, 0x9747b28c);
  uint32_t b = murmurhash2(buffer, len, a);
  uint64_t x;
  uint64_t i;

  for (i = 0; i < bloom->hashes; i++) {
    x = (a + b*i) % bloom->bits;
    if (test_bit_set_bit(bloom->bf, x, add)) {
      hits++;
    } else if (!add) {
      // Don't care about the presence of all the bits. Just our own.
      return 0;
    }
  }

  if (hits == bloom->hashes) {
    return 1;                // 1 == element already in (or collision)
  }

  return 0;
}


int8_t bloom_init2(struct bloom * bloom, uint32_t entries, double error)
{
  if (sizeof(uint64_t) < 8) {
    ESP_LOGE(TAG_BLOOM, "error: libbloom will not function correctly because");
    ESP_LOGE(TAG_BLOOM, "sizeof(uint64_t) == %llu", (uint64_t)sizeof(uint64_t));
    exit(1);
  }

  memset(bloom, 0, sizeof(struct bloom));

  if (entries < 10 || error <= 0 || error >= 1) {
    return 1;
  }

  bloom->entries = entries;
  bloom->error = error;

  double num = -log(bloom->error);
  double denom = 0.480453013918201; // ln(2)^2
  bloom->bpe = (num / denom);

  long double dentries = (long double)entries;
  long double allbits = dentries * bloom->bpe;
  bloom->bits = (uint64_t)allbits;

  if (bloom->bits % 8) {
    bloom->bytes = (bloom->bits / 8) + 1;
  } else {
    bloom->bytes = bloom->bits / 8;
  }

  bloom->hashes = (uint8_t)ceil(0.693147180559945 * bloom->bpe); // ln(2)

  bloom->bf = (uint8_t *)calloc(bloom->bytes, sizeof(uint8_t));
  if (bloom->bf == NULL) {                                   
    ESP_LOGE(TAG_BLOOM, "Calloc failed");
    return 1;
  }                                                          

  bloom->ready = 1;

  return 0;
}


int8_t bloom_check(struct bloom * bloom, const void * buffer, uint32_t len)
{
  return bloom_check_add(bloom, buffer, len, 0);
}


int8_t bloom_add(struct bloom * bloom, const void * buffer, uint32_t len)
{
  return bloom_check_add(bloom, buffer, len, 1);
}

void bloom_print(struct bloom * bloom)
{
  ESP_LOGW(TAG_BLOOM, "bloom at %p", (void *)bloom);
  if (!bloom->ready) { ESP_LOGW(TAG_BLOOM, " *** NOT READY ***"); }
  ESP_LOGW(TAG_BLOOM, " ->entries = %lu", bloom->entries);
  ESP_LOGW(TAG_BLOOM, " ->error = %lf", bloom->error);
  ESP_LOGW(TAG_BLOOM, " ->bits = %llu", bloom->bits);
  ESP_LOGW(TAG_BLOOM, " ->bits per elem = %lf", bloom->bpe);
  ESP_LOGW(TAG_BLOOM, " ->bytes = %u", bloom->bytes);
  ESP_LOGW(TAG_BLOOM, " ->hash functions = %d", bloom->hashes);
}


void bloom_free(struct bloom * bloom)
{
  if (bloom->ready) {
    free(bloom->bf);
  }
  bloom->ready = 0;
}


int8_t bloom_reset(struct bloom * bloom)
{
  if (!bloom->ready) return 1;
  memset(bloom->bf, 0, bloom->bytes);
  return 0;
}


int8_t bloom_merge(struct bloom * bloom_dest, struct bloom * bloom_src)
{
  if (bloom_dest->ready == 0) {
    ESP_LOGE(TAG_BLOOM, "bloom at %p not initialized!", (void *)bloom_dest);
    return -1;
  }

  if (bloom_src->ready == 0) {
    ESP_LOGE(TAG_BLOOM, "bloom at %p not initialized!", (void *)bloom_src);
    return -1;
  }

  if (bloom_dest->entries != bloom_src->entries) {
    return 1;
  }

  if (bloom_dest->error != bloom_src->error) {
    return 1;
  }

  // Not really possible if properly used but check anyway to avoid the
  // possibility of buffer overruns.
  if (bloom_dest->bytes != bloom_src->bytes) {
    return 1;                                                // LCOV_EXCL_LINE
  }

  uint64_t p;
  for (p = 0; p < bloom_dest->bytes; p++) {
    bloom_dest->bf[p] |= bloom_src->bf[p];
  }

  return 0;
}