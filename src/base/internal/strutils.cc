#include <cctype>
#include <string.h>
#include <unicode/brkiter.h>

#include "thirdparty/utf8_decoder_dfa/decoder.h"
#include "thirdparty/widecharwidth/widechar_width.h"

#include "strutils.h"

// Addition by Yucong Sun

bool u8_validate(const char *s) {
  auto p = (const uint8_t *)s;
  uint32_t codepoint, state = 0;

  while (*p) decode(&state, &codepoint, *p++);

  return state == UTF8_ACCEPT;
}

bool u8_validate(const uint8_t *s, size_t len) {
  auto end = s + len;
  uint32_t codepoint, state = 0;

  while (s < end && *s) decode(&state, &codepoint, *s++);

  return state == UTF8_ACCEPT;
}

void u8_sanitize(char *src) {
  int total = strlen(src);
  int32_t src_offset = 0;
  int32_t written = 0;
  UChar32 c = -1;
  U8_NEXT(src, src_offset, -1, c);
  while (c != 0) {
    UBool isError = FALSE;
    U8_APPEND((uint8_t *)src, written, total, c > 0 ? c : 0xfffd, isError);
    if (isError == TRUE) break;

    U8_NEXT((uint8_t *)src, src_offset, -1, c);
  }
}

bool u8_egc_count(const char *src, size_t *count) {
  UErrorCode status = U_ZERO_ERROR;
  size_t total = 0;
  UText text = UTEXT_INITIALIZER;

  utext_openUTF8(&text, src, -1, &status);
  if (!U_SUCCESS(status)) {
    utext_close(&text);
    return false;
  }

  /* create an iterator for graphemes */
  std::unique_ptr<icu::BreakIterator> brk(
      icu::BreakIterator::createCharacterInstance(icu::Locale::getDefault(), status));
  if (!U_SUCCESS(status)) {
    utext_close(&text);
    return false;
  }
  brk->setText(&text, status);
  if (!U_SUCCESS(status)) {
    utext_close(&text);
    return false;
  }
  brk->first();
  while (brk->next() != icu::BreakIterator::DONE) ++total;

  *count = total;

  utext_close(&text);
  return true;
}

// Return the egc at given index of src, if it is an single code point
UChar32 u8_egc_index_as_single_codepoint(const char *src, int32_t index) {
  UErrorCode status = U_ZERO_ERROR;
  int32_t pos = -1;
  UText text = UTEXT_INITIALIZER;

  utext_openUTF8(&text, src, -1, &status);
  if (!U_SUCCESS(status)) {
    utext_close(&text);
    return -1;
  }

  std::unique_ptr<icu::BreakIterator> brk(
      icu::BreakIterator::createCharacterInstance(icu::Locale::getDefault(), status));
  if (!U_SUCCESS(status)) {
    utext_close(&text);
    return -1;
  }

  brk->setText(&text, status);
  if (!U_SUCCESS(status)) {
    utext_close(&text);
    return -1;
  }

  pos = brk->first();
  while (index-- > 0 && pos >= 0) {
    pos = brk->next();
  }

  // index is end-of-string
  if (src[pos] == 0) {
    utext_close(&text);
    return 0;
  }

  UChar32 res = U_SENTINEL;
  auto next_pos = brk->next();
  if (next_pos >= 0) {
    if (next_pos - pos <= U8_MAX_LENGTH) {
      U8_NEXT((const uint8_t *)src, pos, -1, res);
    }
  }

  utext_close(&text);
  return res;
}

// Copy string src to dest, replacing character at index to c. Assuming dst is already allocated.
void u8_copy_and_replace_codepoint_at(const char *src, char *dst, int32_t index, UChar32 c) {
  int32_t src_offset = u8_egc_index_to_offset(src, index);
  int32_t dst_offset = 0;

  memcpy(dst, src, src_offset);
  dst_offset = src_offset;
  U8_APPEND_UNSAFE(dst, dst_offset, c);

  U8_FWD_1_UNSAFE(src, src_offset);
  strcpy((char *)dst + dst_offset, (const char *)src + src_offset);
}

// Get the byte offset to the egc index, doesn't check validity or bounds.
int32_t u8_egc_index_to_offset(const char *src, int32_t index) {
  UErrorCode status = U_ZERO_ERROR;
  int32_t pos = -1;
  UText text = UTEXT_INITIALIZER;

  utext_openUTF8(&text, src, -1, &status);
  if (!U_SUCCESS(status)) {
    utext_close(&text);
    return -1;
  }

  std::unique_ptr<icu::BreakIterator> brk(
      icu::BreakIterator::createCharacterInstance(icu::Locale::getDefault(), status));
  if (!U_SUCCESS(status)) {
    utext_close(&text);
    return -1;
  }

  brk->setText(&text, status);
  if (!U_SUCCESS(status)) {
    utext_close(&text);
    return -1;
  }

  pos = brk->first();
  while (index-- > 0 && pos >= 0) {
    pos = brk->next();
  }

  utext_close(&text);
  return pos;
}

// same as strncpy, copy up to maxlen bytes but will not copy broken characters.
int32_t u8_strncpy(uint8_t *dest, const uint8_t *src, const int32_t maxlen) {
  int32_t src_offset = 0;
  int32_t written = 0;
  while (written <= maxlen) {
    UChar32 c = 0;
    U8_NEXT(src, src_offset, maxlen, c);
    if (c <= 0) break;
    UBool isError = FALSE;
    U8_APPEND(dest, written, maxlen, c, isError);
    if (isError == TRUE) break;
  }
  return written;
}

// From ICU 61
#ifndef U8_TRUNCATE_IF_INCOMPLETE
/**
 * Internal bit vector for 3-byte UTF-8 validity check, for use in U8_IS_VALID_LEAD3_AND_T1.
 * Each bit indicates whether one lead byte + first trail byte pair starts a valid sequence.
 * Lead byte E0..EF bits 3..0 are used as byte index,
 * first trail byte bits 7..5 are used as bit index into that byte.
 * @see U8_IS_VALID_LEAD3_AND_T1
 * @internal
 */
#define U8_LEAD3_T1_BITS "\x20\x30\x30\x30\x30\x30\x30\x30\x30\x30\x30\x30\x30\x10\x30\x30"

/**
 * Internal 3-byte UTF-8 validity check.
 * Non-zero if lead byte E0..EF and first trail byte 00..FF start a valid sequence.
 * @internal
 */
#define U8_IS_VALID_LEAD3_AND_T1(lead, t1) (U8_LEAD3_T1_BITS[(lead)&0xf]&(1<<((uint8_t)(t1)>>5)))

/**
 * Internal bit vector for 4-byte UTF-8 validity check, for use in U8_IS_VALID_LEAD4_AND_T1.
 * Each bit indicates whether one lead byte + first trail byte pair starts a valid sequence.
 * First trail byte bits 7..4 are used as byte index,
 * lead byte F0..F4 bits 2..0 are used as bit index into that byte.
 * @see U8_IS_VALID_LEAD4_AND_T1
 * @internal
 */
#define U8_LEAD4_T1_BITS "\x00\x00\x00\x00\x00\x00\x00\x00\x1E\x0F\x0F\x0F\x00\x00\x00\x00"

/**
 * Internal 4-byte UTF-8 validity check.
 * Non-zero if lead byte F0..F4 and first trail byte 00..FF start a valid sequence.
 * @internal
 */
#define U8_IS_VALID_LEAD4_AND_T1(lead, t1) (U8_LEAD4_T1_BITS[(uint8_t)(t1)>>4]&(1<<((lead)&7)))

/**
 * If the string ends with a UTF-8 byte sequence that is valid so far
 * but incomplete, then reduce the length of the string to end before
 * the lead byte of that incomplete sequence.
 * For example, if the string ends with E1 80, the length is reduced by 2.
 *
 * In all other cases (the string ends with a complete sequence, or it is not
 * possible for any further trail byte to extend the trailing sequence)
 * the length remains unchanged.
 *
 * Useful for processing text split across multiple buffers
 * (save the incomplete sequence for later)
 * and for optimizing iteration
 * (check for string length only once per character).
 *
 * "Safe" macro, checks for illegal sequences and for string boundaries.
 * Unlike U8_SET_CP_START(), this macro never reads s[length].
 *
 * (In UTF-16, simply check for U16_IS_LEAD(last code unit).)
 *
 * @param s const uint8_t * string
 * @param start int32_t starting string offset (usually 0)
 * @param length int32_t string length (usually start<=length)
 * @see U8_SET_CP_START
 * @stable ICU 61
 */
#define U8_TRUNCATE_IF_INCOMPLETE(s, start, length)                                   \
  do {                                                                                \
    if ((length) > (start)) {                                                         \
      uint8_t __b1 = s[(length)-1];                                                   \
      if (U8_IS_SINGLE(__b1)) {                                                       \
        /* common ASCII character */                                                  \
      } else if (U8_IS_LEAD(__b1)) {                                                  \
        --(length);                                                                   \
      } else if (U8_IS_TRAIL(__b1) && ((length)-2) >= (start)) {                      \
        uint8_t __b2 = s[(length)-2];                                                 \
        if (0xe0 <= __b2 && __b2 <= 0xf4) {                                           \
          if (__b2 < 0xf0 ? U8_IS_VALID_LEAD3_AND_T1(__b2, __b1)                      \
                          : U8_IS_VALID_LEAD4_AND_T1(__b2, __b1)) {                   \
            (length) -= 2;                                                            \
          }                                                                           \
        } else if (U8_IS_TRAIL(__b2) && ((length)-3) >= (start)) {                    \
          uint8_t __b3 = s[(length)-3];                                               \
          if (0xf0 <= __b3 && __b3 <= 0xf4 && U8_IS_VALID_LEAD4_AND_T1(__b3, __b2)) { \
            (length) -= 3;                                                            \
          }                                                                           \
        }                                                                             \
      }                                                                               \
    }                                                                                 \
  } while (false)
#endif

// truncate strlen() to last valid codepoint
size_t u8_truncate(const uint8_t *src, size_t len) {
  int32_t res = len;
  U8_TRUNCATE_IF_INCOMPLETE(src, 0, res);
  return res;
}

// Total width of characters(grapheme cluster). Also adjust for east asain full width characters
size_t u8_width(const char *src) {
  size_t total = 0;
  int32_t src_offset = 0;

  UChar32 c = 0;
  UChar32 prev = 0;
  for (;;) {
    prev = c;
    U8_NEXT(src, src_offset, -1, c);
    if (c < 0) c = 0xfffd;
    if (c == 0) break;
    if (c == 0x200d || prev == 0x200d) {  // zwj, skip the next character
      continue;
    }
    auto width = widechar_wcwidth(c);
    if (width > 0) {
      total += width;
    } else if (width == widechar_ambiguous) {
      total += 2;
    }
  }
  return total;
}
