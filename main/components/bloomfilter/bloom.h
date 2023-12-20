/*
 *  Copyright (c) 2012-2022, Jyri J. Virkki
 *  All rights reserved.
 *
 *  This file is under BSD license. See LICENSE file.
 */

#ifndef _BLOOM_H
#define _BLOOM_H

/** ***************************************************************************
 * Structure to keep track of one bloom filter.  Caller needs to
 * allocate this and pass it to the functions below. First call for
 * every struct must be to bloom_init().
 *
 */
typedef struct bloom
{
  // These fields are part of the public interface of this structure.
  // Client code may read these values if desired. Client code MUST NOT
  // modify any of these.
  uint32_t entries;
  uint64_t bits;
  uint64_t bytes;
  uint16_t hashes;
  double error;

  // Fields below are private to the implementation. These may go away or
  // change incompatibly at any moment. Client code MUST NOT access or rely
  // on these.
  uint16_t ready;
  uint16_t major;
  uint16_t minor;
  double bpe;
  uint8_t * bf;
} __attribute__((packed)) bloom_t;


/** ***************************************************************************
 * Initialize the bloom filter for use.
 *
 * The filter is initialized with a bit field and number of hash functions
 * according to the computations from the wikipedia entry:
 *     http://en.wikipedia.org/wiki/Bloom_filter
 *
 * Optimal number of bits is:
 *     bits = (entries * ln(error)) / ln(2)^2
 *
 * Optimal number of hash functions is:
 *     hashes = bpe * ln(2)
 *
 * Parameters:
 * -----------
 *     bloom   - Pointer to an allocated struct bloom (see above).
 *     entries - The expected number of entries which will be inserted.
 *               Must be at least 200 (in practice, likely much larger).
 *     error   - Probability of collision (as long as entries are not
 *               exceeded).
 *
 * Return:
 * -------
 *     0 - on success
 *     1 - on failure
 *
 */
int8_t bloom_init2(struct bloom * bloom, uint32_t entries, double error);


/** ***************************************************************************
 * Check if the given element is in the bloom filter. Remember this may
 * return false positive if a collision occurred.
 *
 * Parameters:
 * -----------
 *     bloom  - Pointer to an allocated struct bloom (see above).
 *     buffer - Pointer to buffer containing element to check.
 *     len    - Size of 'buffer'.
 *
 * Return:
 * -------
 *     0 - element is not present
 *     1 - element is present (or false positive due to collision)
 *    -1 - bloom not initialized
 *
 */
int8_t bloom_check(struct bloom * bloom, const void * buffer, uint32_t len);


/** ***************************************************************************
 * Add the given element to the bloom filter.
 * The return code indicates if the element (or a collision) was already in,
 * so for the common check+add use case, no need to call check separately.
 *
 * Parameters:
 * -----------
 *     bloom  - Pointer to an allocated struct bloom (see above).
 *     buffer - Pointer to buffer containing element to add.
 *     len    - Size of 'buffer'.
 *
 * Return:
 * -------
 *     0 - element was not present and was added
 *     1 - element (or a collision) had already been added previously
 *    -1 - bloom not initialized
 *
 */
int8_t bloom_add(struct bloom * bloom, const void * buffer, uint32_t len);

/** ***************************************************************************
 * 
 * Serializes a bloom filter into one block of data.
 * Returns pointer to block of serialized data.
 *
 * Parameters:
 * -----------
 *     bloom  - Pointer to an allocated struct bloom (see above).
 *     block  - Pointer to serialized bloom filter after serialization.
 *
 * Return:
 * -------
 *  NULL - serialization of bloom filter failed
 *       - else the address to the block of serialized data
 *
 * Author: Daniel Fronk
 */
uint8_t* bloom_serialize(struct bloom * bloom);

/** ***************************************************************************
 * Print (to stdout) info about this bloom filter. Debugging aid.
 *
 */
void bloom_print(struct bloom * bloom);


/** ***************************************************************************
 * Deallocate internal storage.
 *
 * Upon return, the bloom struct is no longer usable. You may call bloom_init
 * again on the same struct to reinitialize it again.
 *
 * Parameters:
 * -----------
 *     bloom  - Pointer to an allocated struct bloom (see above).
 *
 * Return: none
 *
 */
void bloom_free(struct bloom * bloom);


/** ***************************************************************************
 * Erase internal storage.
 *
 * Erases all elements. Upon return, the bloom struct returns to its initial
 * (initialized) state.
 *
 * Parameters:
 * -----------
 *     bloom  - Pointer to an allocated struct bloom (see above).
 *
 * Return:
 *     0 - on success
 *     1 - on failure
 *
 */
int8_t bloom_reset(struct bloom * bloom);


/** ***************************************************************************
 * Merge two compatible bloom filters.
 *
 * On success, bloom_dest will contain all elements of bloom_src in addition
 * to its own. The bloom_src bloom filter is never modified.
 *
 * Both bloom_dest and bloom_src must be initialized and both must have
 * identical parameters.
 *
 * Parameters:
 * -----------
 *     bloom_dest - will contain the merged elements from bloom_src
 *     bloom_src  - its elements will be merged into bloom_dest
 *
 * Return:
 * -------
 *     0 - on success
 *     1 - incompatible bloom filters
 *    -1 - bloom not initialized
 *
 */
int8_t bloom_merge(struct bloom * bloom_dest, struct bloom * bloom_src);

#endif