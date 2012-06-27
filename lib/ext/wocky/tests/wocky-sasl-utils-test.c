/*
 * wocky-sasl-utils-test.c - Test sasl utilities
 * Copyright (C) 2010 Sjoerd Simons <sjoerd@luon.net>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdio.h>
#include <wocky/wocky.h>

typedef struct {
  guint8 *key;
  gsize key_len;
  guint8 *data;
  gsize data_len;
  guint8 *result;
} digest_test;

/* HMAC-SHA1 test vectors as per RFC 2202 */

/* Test 1 */
guint8 key_sha1_test1[] = {
    0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b,
    0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b };
guint8 result_sha1_test1[] = {
    0xb6, 0x17, 0x31, 0x86, 0x55, 0x05, 0x72, 0x64, 0xe2, 0x8b,
    0xc0, 0xb6, 0xfb, 0x37, 0x8c, 0x8e, 0xf1, 0x46, 0xbe, 0x00 };

/* Test 2 */
guint8 result_sha1_test2[] = {
    0xef, 0xfc, 0xdf, 0x6a, 0xe5, 0xeb, 0x2f, 0xa2, 0xd2, 0x74,
    0x16, 0xd5, 0xf1, 0x84, 0xdf, 0x9c, 0x25, 0x9a, 0x7c, 0x79 };

/* Test 3 */
guint8 key_sha1_test3[] = {
    0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa,
    0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa };
guint8 data_sha1_test3[] = {
    0xdd, 0xdd, 0xdd, 0xdd, 0xdd, 0xdd, 0xdd, 0xdd, 0xdd, 0xdd,
    0xdd, 0xdd, 0xdd, 0xdd, 0xdd, 0xdd, 0xdd, 0xdd, 0xdd, 0xdd,
    0xdd, 0xdd, 0xdd, 0xdd, 0xdd, 0xdd, 0xdd, 0xdd, 0xdd, 0xdd,
    0xdd, 0xdd, 0xdd, 0xdd, 0xdd, 0xdd, 0xdd, 0xdd, 0xdd, 0xdd,
    0xdd, 0xdd, 0xdd, 0xdd, 0xdd, 0xdd, 0xdd, 0xdd, 0xdd, 0xdd };
guint8 result_sha1_test3[] = {
    0x12, 0x5d, 0x73, 0x42, 0xb9, 0xac, 0x11, 0xcd, 0x91, 0xa3,
    0x9a, 0xf4, 0x8a, 0xa1, 0x7b, 0x4f, 0x63, 0xf1, 0x75, 0xd3 };

/* Test 4 */
guint8 key_sha1_test4[] = {
    0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a,
    0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10, 0x11, 0x12, 0x13, 0x14,
    0x15, 0x16, 0x17, 0x18, 0x19 };
guint8 data_sha1_test4[] = {
    0xcd, 0xcd, 0xcd, 0xcd, 0xcd, 0xcd, 0xcd, 0xcd, 0xcd, 0xcd,
    0xcd, 0xcd, 0xcd, 0xcd, 0xcd, 0xcd, 0xcd, 0xcd, 0xcd, 0xcd,
    0xcd, 0xcd, 0xcd, 0xcd, 0xcd, 0xcd, 0xcd, 0xcd, 0xcd, 0xcd,
    0xcd, 0xcd, 0xcd, 0xcd, 0xcd, 0xcd, 0xcd, 0xcd, 0xcd, 0xcd,
    0xcd, 0xcd, 0xcd, 0xcd, 0xcd, 0xcd, 0xcd, 0xcd, 0xcd, 0xcd };
guint8 result_sha1_test4[] = {
    0x4c, 0x90, 0x07, 0xf4, 0x02, 0x62, 0x50, 0xc6, 0xbc, 0x84,
    0x14, 0xf9, 0xbf, 0x50, 0xc8, 0x6c, 0x2d, 0x72, 0x35, 0xda };

/* Test 5 */
guint8 key_sha1_test5[] = {
    0x0c, 0x0c, 0x0c, 0x0c, 0x0c, 0x0c, 0x0c, 0x0c, 0x0c, 0x0c,
    0x0c, 0x0c, 0x0c, 0x0c, 0x0c, 0x0c, 0x0c, 0x0c, 0x0c, 0x0c };
guint8 result_sha1_test5[] = {
    0x4c, 0x1a, 0x03, 0x42, 0x4b, 0x55, 0xe0, 0x7f, 0xe7, 0xf2,
    0x7b, 0xe1, 0xd5, 0x8b, 0xb9, 0x32, 0x4a, 0x9a, 0x5a, 0x04 };

/* Test 6 & 7*/
guint8 key_sha1_test6_7[] = {
    0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa,
    0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa,
    0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa,
    0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa,
    0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa,
    0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa,
    0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa,
    0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa };

guint8 result_sha1_test6[] = {
    0xaa, 0x4a, 0xe5, 0xe1, 0x52, 0x72, 0xd0, 0x0e, 0x95, 0x70,
    0x56, 0x37, 0xce, 0x8a, 0x3b, 0x55, 0xed, 0x40, 0x21, 0x12 };

guint8 result_sha1_test7[] = {
    0xe8, 0xe9, 0x9d, 0xf, 0x45, 0x23, 0x7d, 0x78, 0x6d, 0x6b,
    0xba, 0xa7, 0x96, 0x5c, 0x78, 0x8, 0xbb, 0xff, 0x1a, 0x91 };


digest_test hmac_sha1_tests[] = {
  { key_sha1_test1, 20, (guint8 *) "Hi There", 8, result_sha1_test1 },
  {
    (guint8 *) "Jefe", 4, (guint8 *) "what do ya want for nothing?", 28,
    result_sha1_test2
  },
  { key_sha1_test3, 20, data_sha1_test3, 50, result_sha1_test3 },
  { key_sha1_test4, 25, data_sha1_test4, 50, result_sha1_test4 },
  {
    key_sha1_test5, 20,
    (guint8 *) "Test With Truncation", 20,
    result_sha1_test5
  },
  {
    key_sha1_test6_7, 80,
    (guint8 *) "Test Using Larger Than Block-Size Key - Hash Key First", 54,
    result_sha1_test6
  },
  {
    key_sha1_test6_7, 80,
    (guint8 *) "Test Using Larger Than Block-Size Key and Larger" \
               " Than One Block-Size Data",
    73,
    result_sha1_test7,
  },
  { NULL, 0, NULL, 0, NULL },
};

static void
test_sasl_utils_hmac_sha1 (digest_test *t)
{
  GByteArray *result = sasl_calculate_hmac_sha1 (t->key, t->key_len,
    t->data, t->data_len);
  int i;

  for (i = 0; i < g_checksum_type_get_length (G_CHECKSUM_SHA1); i++)
    g_assert_cmphex (result->data[i], ==, t->result[i]);

  g_byte_array_unref (result);
}

int
main (int argc,
    char **argv)
{
  int i;

#if !GLIB_CHECK_VERSION (2, 31, 0)
  g_thread_init (NULL);
#endif

  g_test_init (&argc, &argv, NULL);

  for (i = 0 ; hmac_sha1_tests[i].key_len > 0 ; i++)
    {
      gchar *name = g_strdup_printf ("/sasl-utils/hmac-sha1-%d", i + 1);

      g_test_add_data_func (name,
        hmac_sha1_tests + i,
        (void (*)(const void *)) test_sasl_utils_hmac_sha1);
      g_free (name);
    }

  return g_test_run ();
}
