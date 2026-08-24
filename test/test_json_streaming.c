// This file is used to test streaming JSON reader and writer.

#include "yyjson.h"
#include "yy_test_utils.h"

#if !YYJSON_DISABLE_STREAMING

#define TEST_DEPTH                 ((size_t)1024)

#if !YYJSON_DISABLE_STREAMING && !YYJSON_DISABLE_READER
#define TEST_SR_MIN_WINDOW         YYJSON_SR_MIN_WINDOW
#define TEST_SR_RECOMMENDED_WINDOW ((size_t)16 << 10)
#define TEST_SR_MAX_WINDOW         YYJSON_SR_MAX_WINDOW

/*==============================================================================
 * MARK: - Reader Input
 *============================================================================*/

typedef enum read_input_kind {
    READ_INPUT_INSITU,
    READ_INPUT_MEMORY,
    READ_INPUT_STREAM
} read_input_kind;

typedef struct read_input_mode {
    read_input_kind kind;
    size_t chunk;
} read_input_mode;

typedef struct read_feed {
    const char *dat;
    size_t len;
    size_t pos;
    size_t step;
} read_feed;

static const read_input_mode read_modes[] = {
    { READ_INPUT_INSITU, 0 },
    { READ_INPUT_MEMORY, 0 },
    { READ_INPUT_STREAM, 1 },
    { READ_INPUT_STREAM, 2 },
    { READ_INPUT_STREAM, 3 },
    { READ_INPUT_STREAM, 7 },
    { READ_INPUT_STREAM, 64 },
    { READ_INPUT_STREAM, 4096 }
};

static size_t read_feed_fn(void *ctx, void *dst, size_t cap) {
    read_feed *f = (read_feed *)ctx;
    size_t left = f->len - f->pos;
    size_t n = left < f->step ? left : f->step;
    if (n > cap) n = cap;
    memcpy(dst, f->dat + f->pos, n);
    f->pos += n;
    return n;
}

static char *copy_padded(const char *json, size_t *len_out) {
    size_t len = strlen(json);
    char *dat = (char *)malloc(len + YYJSON_PADDING_SIZE);
    yy_assert(dat != NULL);
    memcpy(dat, json, len);
    memset(dat + len, 0, YYJSON_PADDING_SIZE);
    if (len_out) *len_out = len;
    return dat;
}

static void assert_sv_terminated(yyjson_sv sv) {
    yy_assert(sv.ptr != NULL);
    yy_assert(sv.ptr[sv.len] == '\0');
}

static void assert_raw_val(const yyjson_val *val, const char *raw) {
    size_t len = strlen(raw);
    yy_assert(yyjson_is_raw(val));
    yy_assert(yyjson_get_len(val) == len);
    yy_assert(memcmp(yyjson_get_raw(val), raw, len) == 0);
}

static read_input_mode g_read_mode;
static read_feed g_read_feed;
static char g_reader_buf[12 * 1024];

static bool init_reader_mode(yyjson_sr *sr, char *input, size_t len,
                             read_input_mode mode, read_feed *feed,
                             void *buf, size_t buf_cap,
                             yyjson_read_flag flg) {
    if (mode.kind == READ_INPUT_INSITU) {
        return yyjson_sr_init_mem(sr, input, len, buf, buf_cap, flg | YYJSON_READ_INSITU);
    } else if (mode.kind == READ_INPUT_MEMORY) {
        return yyjson_sr_init_mem(sr, input, len, buf, buf_cap, flg);
    }

    feed->dat = input;
    feed->len = len;
    feed->pos = 0;
    feed->step = mode.chunk;
    return yyjson_sr_init_fn(sr, read_feed_fn, feed, buf, buf_cap, flg);
}

static bool init_reader_flags(yyjson_sr *sr, char *input, size_t len,
                              yyjson_read_flag flg) {
    return init_reader_mode(sr, input, len, g_read_mode, &g_read_feed, g_reader_buf,
                            sizeof(g_reader_buf), flg);
}

static bool init_reader(yyjson_sr *sr, char *input, size_t len) {
    return init_reader_flags(sr, input, len, 0);
}


/*==============================================================================
 * MARK: - Reader Common Paths
 *============================================================================*/

static void test_sample(void) {
    static const int64_t expected_hits[] = {2, 2, 1, 3};
    size_t len;
    char *json = copy_padded("{\"name\":\"Mash\",\"star\":4,\"hits\":[2,2,1,3],"
        "\"pi\":3.25,\"ok\":true,\"none\":null,\"neg\":-7}", &len);
    yyjson_sr sr;
    size_t hit = 0;

    yy_assert(init_reader(&sr, json, len));
    yy_assert(yyjson_sr_obj_begin(&sr));
    yyjson_sv sv = yyjson_sr_obj_read_str(&sr, "name");
    yy_assert(yyjson_sv_equals_str(sv, "Mash"));

    yy_assert(yyjson_sr_obj_find(&sr, "star"));
    yy_assert(yyjson_sr_read_uint(&sr) == 4);

    yy_assert(yyjson_sr_obj_find(&sr, "hits"));
    yy_assert(yyjson_sr_arr_begin(&sr));
    while (yyjson_sr_arr_next(&sr)) {
        yy_assert(hit < yy_nelems(expected_hits));
        yy_assert(yyjson_sr_read_sint(&sr) == expected_hits[hit++]);
    }
    yy_assert(yyjson_sr_arr_end(&sr));
    yy_assert(hit == yy_nelems(expected_hits));

    yy_assert(yyjson_sr_obj_find(&sr, "pi"));
    yy_assert(yyjson_sr_read_real(&sr) == 3.25);
    yy_assert(yyjson_sr_obj_read_bool(&sr, "ok"));
    yyjson_sr_obj_read_null(&sr, "none");
    yy_assert(yyjson_sr_obj_read_sint(&sr, "neg") == -7);
    yy_assert(yyjson_sr_obj_end(&sr));
    yy_assert(yyjson_sr_finish(&sr));

    free(json);
}

static void test_generic_scalars(void) {
    size_t len;
    char *json = copy_padded(
        "[0,18446744073709551615,-7,1.25,true,false,null,\"a\\nb\"]",
        &len);
    yyjson_sr sr;
    yyjson_val val;

    yy_assert(init_reader(&sr, json, len));
    yy_assert(yyjson_sr_arr_begin(&sr));

    yy_assert(yyjson_sr_arr_next(&sr));
    val = yyjson_sr_read_num(&sr);
    yy_assert(yyjson_sr_ok(&sr));
    yy_assert(yyjson_is_uint(&val) && yyjson_get_uint(&val) == 0);

    yy_assert(yyjson_sr_arr_next(&sr));
    val = yyjson_sr_read_num(&sr);
    yy_assert(yyjson_sr_ok(&sr));
    yy_assert(yyjson_is_uint(&val));
    yy_assert(yyjson_get_uint(&val) == UINT64_MAX);

    yy_assert(yyjson_sr_arr_next(&sr));
    val = yyjson_sr_read_scalar(&sr);
    yy_assert(yyjson_sr_ok(&sr));
    yy_assert(yyjson_is_sint(&val) && yyjson_get_sint(&val) == -7);

    yy_assert(yyjson_sr_arr_next(&sr));
    val = yyjson_sr_read_scalar(&sr);
    yy_assert(yyjson_sr_ok(&sr));
    yy_assert(yyjson_is_real(&val) && yyjson_get_real(&val) == 1.25);

    yy_assert(yyjson_sr_arr_next(&sr));
    val = yyjson_sr_read_scalar(&sr);
    yy_assert(yyjson_sr_ok(&sr));
    yy_assert(yyjson_is_true(&val));

    yy_assert(yyjson_sr_arr_next(&sr));
    val = yyjson_sr_read_scalar(&sr);
    yy_assert(yyjson_sr_ok(&sr));
    yy_assert(yyjson_is_false(&val));

    yy_assert(yyjson_sr_arr_next(&sr));
    val = yyjson_sr_read_scalar(&sr);
    yy_assert(yyjson_sr_ok(&sr));
    yy_assert(yyjson_is_null(&val));

    yy_assert(yyjson_sr_arr_next(&sr));
    val = yyjson_sr_read_scalar(&sr);
    yy_assert(yyjson_sr_ok(&sr));
    yy_assert(yyjson_is_str(&val));
    yy_assert(yyjson_get_len(&val) == 3);
    yy_assert(memcmp(yyjson_get_str(&val), "a\nb", 3) == 0);
    yy_assert(yyjson_get_str(&val)[yyjson_get_len(&val)] == '\0');

    yy_assert(!yyjson_sr_arr_next(&sr));
    yy_assert(yyjson_sr_arr_end(&sr));
    yy_assert(yyjson_sr_finish(&sr));
    free(json);

    json = copy_padded("[]", &len);
    yy_assert(init_reader(&sr, json, len));
    val = yyjson_sr_read_scalar(&sr);
    yy_assert(yyjson_get_type(&val) == YYJSON_TYPE_NONE);
    yy_assert(!yyjson_sr_ok(&sr));
    free(json);

    json = copy_padded("\"hello\"", &len);
    yy_assert(init_reader(&sr, json, len));
    yy_assert(yyjson_sv_equals_str(yyjson_sr_read_str(&sr), "hello"));
    yy_assert(yyjson_sr_finish(&sr));
    free(json);
}

static void test_number_raw_flags(void) {
    static const char big_int[] = "18446744073709551616";
    size_t len;
    yyjson_sr sr;
    yyjson_val val;

    char *json = copy_padded("[123,18446744073709551616,1e999,-1.25]", &len);
    yy_assert(init_reader_flags(&sr, json, len, YYJSON_READ_BIGNUM_AS_RAW));
    yy_assert(yyjson_sr_arr_begin(&sr));
    yy_assert(yyjson_sr_arr_next(&sr));
    val = yyjson_sr_read_num(&sr);
    yy_assert(yyjson_is_uint(&val) && yyjson_get_uint(&val) == 123);
    yy_assert(yyjson_sr_arr_next(&sr));
    val = yyjson_sr_read_num(&sr);
    assert_raw_val(&val, big_int);
    yy_assert(yyjson_sr_arr_next(&sr));
    val = yyjson_sr_read_scalar(&sr);
    assert_raw_val(&val, "1e999");
    yy_assert(yyjson_sr_arr_next(&sr));
    val = yyjson_sr_read_scalar(&sr);
    yy_assert(yyjson_is_real(&val) && yyjson_get_real(&val) == -1.25);
    yy_assert(!yyjson_sr_arr_next(&sr));
    yy_assert(yyjson_sr_arr_end(&sr));
    yy_assert(yyjson_sr_finish(&sr));
    free(json);

    json = copy_padded("[123,-1.25]", &len);
    yy_assert(init_reader_flags(&sr, json, len, YYJSON_READ_NUMBER_AS_RAW));
    yy_assert(yyjson_sr_arr_begin(&sr));
    yy_assert(yyjson_sr_arr_next(&sr));
    val = yyjson_sr_read_num(&sr);
    assert_raw_val(&val, "123");
    yy_assert(yyjson_sr_arr_next(&sr));
    val = yyjson_sr_read_scalar(&sr);
    assert_raw_val(&val, "-1.25");
    yy_assert(!yyjson_sr_arr_next(&sr));
    yy_assert(yyjson_sr_arr_end(&sr));
    yy_assert(yyjson_sr_finish(&sr));
    free(json);

    json = copy_padded("[42,-7,1.25]", &len);
    yy_assert(init_reader_flags(&sr, json, len, YYJSON_READ_NUMBER_AS_RAW));
    yy_assert(yyjson_sr_arr_begin(&sr));
    yy_assert(yyjson_sr_arr_next(&sr));
    yy_assert(yyjson_sr_read_uint(&sr) == 42);
    yy_assert(yyjson_sr_arr_next(&sr));
    yy_assert(yyjson_sr_read_sint(&sr) == -7);
    yy_assert(yyjson_sr_arr_next(&sr));
    yy_assert(yyjson_sr_read_real(&sr) == 1.25);
    yy_assert(!yyjson_sr_arr_next(&sr));
    yy_assert(yyjson_sr_arr_end(&sr));
    yy_assert(yyjson_sr_finish(&sr));
    free(json);

    json = copy_padded("01", &len);
    yy_assert(init_reader_flags(&sr, json, len, YYJSON_READ_NUMBER_AS_RAW));
    val = yyjson_sr_read_num(&sr);
    yy_assert(yyjson_get_type(&val) == YYJSON_TYPE_NONE);
    yy_assert(!yyjson_sr_ok(&sr));
    free(json);

#if !YYJSON_DISABLE_WRITER
    {
        char output[64];
        yyjson_sw sw;

        json = copy_padded(big_int, &len);
        yy_assert(init_reader_flags(&sr, json, len, YYJSON_READ_BIGNUM_AS_RAW));
        val = yyjson_sr_read_scalar(&sr);
        assert_raw_val(&val, big_int);
        yy_assert(yyjson_sw_init_mem(&sw, output, sizeof(output), 0));
        yy_assert(yyjson_sw_write_scalar(&sw, &val));
        yy_assert(yyjson_sw_finish(&sw));
        yy_assert(yyjson_sw_len(&sw) == len);
        yy_assert(memcmp(output, big_int, len) == 0);
        yy_assert(yyjson_sr_finish(&sr));
        free(json);
    }
#endif
}

static void test_raw_values(void) {
    static const char *expect[] = { "-1.2e3", "\"a\\n\"", "true", "null" };
    size_t len, i = 0;
    char *json = copy_padded("[ \n -1.2e3,\"a\\n\",true,null]", &len);
    yyjson_sr sr;

    yy_assert(init_reader(&sr, json, len));
    yy_assert(yyjson_sr_arr_begin(&sr));
    while (yyjson_sr_arr_next(&sr)) {
        yyjson_sv raw = yyjson_sr_read_raw(&sr);
        yy_assert(i < yy_nelems(expect));
        yy_assert(yyjson_sv_equals_str(raw, expect[i]));
        i++;
    }
    yy_assert(i == yy_nelems(expect));
    yy_assert(yyjson_sr_arr_end(&sr));
    yy_assert(yyjson_sr_finish(&sr));
    free(json);

    json = copy_padded("[{}]", &len);
    yy_assert(init_reader(&sr, json, len));
    yy_assert(yyjson_sr_arr_begin(&sr));
    yy_assert(yyjson_sr_arr_next(&sr));
    yy_assert(!yyjson_sr_read_raw(&sr).ptr);
    yy_assert(!yyjson_sr_ok(&sr));
    free(json);

    json = copy_padded("123", &len);
    yy_assert(init_reader(&sr, json, len));
    yy_assert(yyjson_sv_equals_str(yyjson_sr_read_raw(&sr), "123"));
    yy_assert(yyjson_sr_finish(&sr));
    free(json);
}

static void test_unordered_find(void) {
    static const char text[] = "{\"a\":1,\"skip1\":{\"x\":[1,2,{\"y\":\"s\"}]},\"b\":\"two\",\"skip2\":[true,null,\"z\\\"q\"],\"c\":3}";
    size_t len;
    char *json = copy_padded(text, &len);
    yyjson_sr sr;
    yyjson_read_err err;
    yyjson_sv sv;

    /* find keys out of declaration order */
    yy_assert(init_reader(&sr, json, len));
    yy_assert(yyjson_sr_obj_begin(&sr));
    yy_assert(yyjson_sv_equals_str(yyjson_sr_obj_read_str(&sr, "b"), "two"));
    yy_assert(yyjson_sr_obj_find(&sr, "c"));
    yy_assert(yyjson_sr_read_uint(&sr) == 3);
    yy_assert(yyjson_sr_obj_end(&sr));
    yy_assert(yyjson_sr_finish(&sr));

    /* missing key: false, no error, `}` left for obj_end */
    free(json);
    json = copy_padded(text, &len);
    yy_assert(init_reader(&sr, json, len));
    yy_assert(yyjson_sr_obj_begin(&sr));
    yy_assert(!yyjson_sr_obj_find(&sr, "nope"));
    yy_assert(yyjson_sr_ok(&sr));
    yy_assert(yyjson_sr_obj_end(&sr));
    yy_assert(yyjson_sr_finish(&sr));

    /* fused readers treat their field as required */
    yy_assert(init_reader(&sr, json, len));
    yy_assert(yyjson_sr_obj_begin(&sr));
    yy_assert(yyjson_sr_obj_read_uint(&sr, "nope") == 0);
    yy_assert(!yyjson_sr_ok(&sr));
    yy_assert(yyjson_sr_get_err(&sr, &err) == YYJSON_READ_ERROR_UNEXPECTED_CONTENT);
    yy_assert(err.code == YYJSON_READ_ERROR_UNEXPECTED_CONTENT);

    yy_assert(init_reader(&sr, json, len));
    yy_assert(yyjson_sr_obj_begin(&sr));
    sv = yyjson_sr_obj_read_str(&sr, "nope");
    yy_assert(!sv.ptr && sv.len == 0);
    yy_assert(!yyjson_sr_ok(&sr));

    yy_assert(init_reader(&sr, json, len));
    yy_assert(yyjson_sr_obj_begin(&sr));
    yy_assert(!yyjson_sr_obj_read_bool(&sr, "nope"));
    yy_assert(!yyjson_sr_ok(&sr));

    yy_assert(init_reader(&sr, json, len));
    yy_assert(yyjson_sr_obj_begin(&sr));
    yyjson_sr_obj_read_null(&sr, "nope");
    yy_assert(!yyjson_sr_ok(&sr));
    free(json);

    json = copy_padded("{\"a\\\":x\":1,\"a\\\\\":2}", &len);
    yy_assert(init_reader(&sr, json, len));
    yy_assert(yyjson_sr_obj_begin(&sr));
    yy_assert(yyjson_sr_obj_find(&sr, "a\\"));
    yy_assert(yyjson_sr_read_uint(&sr) == 2);
    yy_assert(yyjson_sr_obj_end(&sr));
    yy_assert(yyjson_sr_finish(&sr));
    free(json);
}

static void test_obj_next(void) {
    size_t len;
    char *json = copy_padded("{\"alpha\":1,\"be\\u0074a\":[2],\"gamma\":{\"g\":3},\"d\":\"x\"}", &len);
    yyjson_sr sr;
    yyjson_sv key;
    int seen = 0;

    yy_assert(init_reader(&sr, json, len));
    yy_assert(yyjson_sr_obj_begin(&sr));
    while (yyjson_sr_obj_next(&sr, &key)) {
        assert_sv_terminated(key);
        switch (seen++) {
        case 0:
            yy_assert(yyjson_sv_equals_str(key, "alpha"));
            yy_assert(yyjson_sr_read_uint(&sr) == 1);
            break;
        case 1:
            yy_assert(yyjson_sv_equals_str(key, "beta")); /* \u0074 = 't' */
            yy_assert(yyjson_sr_skip_value(&sr));
            break;
        case 2:
            yy_assert(yyjson_sv_equals_str(key, "gamma"));
            yy_assert(yyjson_sr_skip_value(&sr));
            break;
        default:
            yy_assert(yyjson_sv_equals_str(key, "d"));
            yy_assert(yyjson_sv_equals_str(yyjson_sr_read_str(&sr), "x"));
            break;
        }
    }
    yy_assert(yyjson_sr_obj_end(&sr));
    yy_assert(yyjson_sr_ok(&sr));
    yy_assert(seen == 4);
    yy_assert(yyjson_sr_finish(&sr));
    free(json);
}

static void test_strings(void) {
    size_t len;
    char *json = copy_padded(
        "[\"\",\"plain\",\"esc\\n\\t\\\"\\\\\\/\\b\\f\\r\","
        "\"\\u0041\\u00e9\\u4e2d\\ud83d\\ude00\",\"caf\xc3\xa9 \xe4\xb8\xad\","
        "\"long-long-long-long-long-long-long-long-long-long-end\"]", &len);
    yyjson_sr sr;
    yyjson_sv sv;

    yy_assert(init_reader(&sr, json, len));
    yy_assert(yyjson_sr_arr_begin(&sr));
    yy_assert(yyjson_sr_arr_next(&sr));
    sv = yyjson_sr_read_str(&sr);
    yy_assert(sv.len == 0 && sv.ptr != NULL);
    assert_sv_terminated(sv);
    yy_assert(yyjson_sr_arr_next(&sr));
    sv = yyjson_sr_read_str(&sr);
    yy_assert(yyjson_sv_equals_str(sv, "plain"));
    assert_sv_terminated(sv);
    yy_assert(yyjson_sr_arr_next(&sr));
    sv = yyjson_sr_read_str(&sr);
    yy_assert(yyjson_sv_equals_str(sv, "esc\n\t\"\\/\b\f\r"));
    assert_sv_terminated(sv);
    yy_assert(yyjson_sr_arr_next(&sr));
    sv = yyjson_sr_read_str(&sr);
    yy_assert(yyjson_sv_equals_str(sv, "A\xc3\xa9\xe4\xb8\xad\xf0\x9f\x98\x80"));
    assert_sv_terminated(sv);
    yy_assert(yyjson_sr_arr_next(&sr));
    sv = yyjson_sr_read_str(&sr);
    yy_assert(yyjson_sv_equals_str(sv, "caf\xc3\xa9 \xe4\xb8\xad"));
    assert_sv_terminated(sv);
    yy_assert(yyjson_sr_arr_next(&sr));
    sv = yyjson_sr_read_str(&sr);
    yy_assert(sv.len == 53);
    assert_sv_terminated(sv);
    yy_assert(!yyjson_sr_arr_next(&sr));
    yy_assert(yyjson_sr_arr_end(&sr));
    yy_assert(yyjson_sr_ok(&sr));
    yy_assert(yyjson_sr_finish(&sr));
    free(json);
}

static void assert_invalid_utf8(const char *src) {
    size_t len;
    char *json = copy_padded(src, &len);
    yyjson_sr sr;
#if YYJSON_DISABLE_UTF8_VALIDATION
    yyjson_sv sv;
#else
    yyjson_read_err err;
#endif

    yy_assert(init_reader(&sr, json, len));
    yy_assert(yyjson_sr_arr_begin(&sr));
    yy_assert(yyjson_sr_arr_next(&sr));
#if YYJSON_DISABLE_UTF8_VALIDATION
    sv = yyjson_sr_read_str(&sr);
    yy_assert(sv.ptr != NULL);
    yy_assert(yyjson_sr_ok(&sr));
    yy_assert(!yyjson_sr_arr_next(&sr));
    yy_assert(yyjson_sr_arr_end(&sr));
    yy_assert(yyjson_sr_finish(&sr));
#else
    (void)yyjson_sr_read_str(&sr);
    yy_assert(!yyjson_sr_ok(&sr));
    yy_assert(yyjson_sr_get_err(&sr, &err) == YYJSON_READ_ERROR_INVALID_STRING);
#endif
    free(json);
}

#if !YYJSON_DISABLE_NON_STANDARD

/* With `YYJSON_READ_ALLOW_INVALID_UNICODE` the bytes come back untouched. */
static void assert_allow_invalid_utf8(const char *src, const char *expect) {
    size_t len, expect_len = strlen(expect);
    char *json = copy_padded(src, &len);
    yyjson_sr sr;
    yyjson_sv sv;

    yy_assert(init_reader_flags(&sr, json, len, YYJSON_READ_ALLOW_INVALID_UNICODE));
    yy_assert(yyjson_sr_arr_begin(&sr));
    yy_assert(yyjson_sr_arr_next(&sr));
    sv = yyjson_sr_read_str(&sr);
    yy_assert(yyjson_sr_ok(&sr));
    yy_assert(sv.len == expect_len);
    yy_assert(memcmp(sv.ptr, expect, expect_len) == 0);
    yy_assert(!yyjson_sr_arr_next(&sr));
    yy_assert(yyjson_sr_arr_end(&sr));
    yy_assert(yyjson_sr_finish(&sr));
    free(json);
}
#endif

static void test_invalid_utf8(void) {
    assert_invalid_utf8("[\"\xe0\x80\x80\"]"); /* overlong */
    assert_invalid_utf8("[\"\xed\xa0\x80\"]"); /* surrogate */
    assert_invalid_utf8("[\"\xf4\x90\x80\x80\"]"); /* past U+10FFFF */
    assert_invalid_utf8("[\"\xe6\x97\"]"); /* truncated */

#if !YYJSON_DISABLE_NON_STANDARD
    /* zero-copy path */
    assert_allow_invalid_utf8("[\"\xe0\x80\x80\"]", "\xe0\x80\x80");
    assert_allow_invalid_utf8("[\"a\xe6\x97 b\"]", "a\xe6\x97 b");
    /* decode path: escape forces a copy, still unvalidated */
    assert_allow_invalid_utf8("[\"\\u0041\xed\xa0\x80\"]", "A\xed\xa0\x80");
    /* valid input is unaffected by the flag */
    assert_allow_invalid_utf8("[\"\xe4\xb8\xad\"]", "\xe4\xb8\xad");
#endif
}

#if !YYJSON_DISABLE_NON_STANDARD
/* An invalid-UTF-8 key is rejected by default and kept with the flag. */
static void test_invalid_utf8_key(void) {
    size_t len;
    char *json = copy_padded("{\"\xc0\x80\":1}", &len);
    yyjson_sr sr;
    yyjson_sv key;

    yy_assert(init_reader_flags(&sr, json, len, YYJSON_READ_ALLOW_INVALID_UNICODE));
    yy_assert(yyjson_sr_obj_begin(&sr));
    yy_assert(yyjson_sr_obj_next(&sr, &key));
    yy_assert(key.len == 2);
    yy_assert(memcmp(key.ptr, "\xc0\x80", 2) == 0);
    yy_assert(yyjson_sr_read_uint(&sr) == 1);
    yy_assert(yyjson_sr_obj_end(&sr));
    yy_assert(yyjson_sr_finish(&sr));

#if !YYJSON_DISABLE_UTF8_VALIDATION
    yy_assert(init_reader(&sr, json, len));
    yy_assert(yyjson_sr_obj_begin(&sr));
    (void)yyjson_sr_obj_next(&sr, &key);
    yy_assert(!yyjson_sr_ok(&sr));
#endif
    free(json);
}
#endif

static void assert_invalid_selected_key(const char *json, const char *key,
                                        bool fused, yyjson_read_code expected) {
    size_t len;
    char *input = copy_padded(json, &len);
    yyjson_sr sr;
    yyjson_read_err err;

    yy_assert(init_reader(&sr, input, len));
    yy_assert(yyjson_sr_obj_begin(&sr));
    if (fused) {
        (void)yyjson_sr_obj_read_uint(&sr, key);
    } else {
        (void)yyjson_sr_obj_find(&sr, key);
    }
    yy_assert(!yyjson_sr_ok(&sr));
    yy_assert(yyjson_sr_get_err(&sr, &err) == expected);
    free(input);
}

static void test_invalid_selected_key(void) {
    assert_invalid_selected_key("{\"bad\nkey\":1}", "bad\nkey", false, YYJSON_READ_ERROR_INVALID_STRING);
    assert_invalid_selected_key("{\"bad\nkey\":1}", "bad\nkey", true, YYJSON_READ_ERROR_INVALID_STRING);
    assert_invalid_selected_key("{\"a\"b\":1}", "a\"b", false, YYJSON_READ_ERROR_UNEXPECTED_CHARACTER);
#if !YYJSON_DISABLE_UTF8_VALIDATION
    assert_invalid_selected_key("{\"\xc0\":1}", "\xc0", false, YYJSON_READ_ERROR_INVALID_STRING);
    assert_invalid_selected_key("{\"\xc0\":1}", "\xc0", true, YYJSON_READ_ERROR_INVALID_STRING);
#endif
}

static void test_numbers(void) {
    size_t len;
    char *json = copy_padded("[0,-0,18446744073709551615,-9223372036854775808,"
        "9223372036854775807,1.5e300,-2.25,0.0001,3e-5,123456789]", &len);
    yyjson_sr sr;
    yyjson_val val;

    yy_assert(init_reader(&sr, json, len));
    yy_assert(yyjson_sr_arr_begin(&sr));
    yy_assert(yyjson_sr_arr_next(&sr));
    yy_assert(yyjson_sr_read_uint(&sr) == 0);
    yy_assert(yyjson_sr_arr_next(&sr));
    yy_assert(yyjson_sr_read_sint(&sr) == 0);
    yy_assert(yyjson_sr_arr_next(&sr));
    yy_assert(yyjson_sr_read_uint(&sr) == UINT64_C(18446744073709551615));
    yy_assert(yyjson_sr_arr_next(&sr));
    yy_assert(yyjson_sr_read_sint(&sr) == INT64_MIN);
    yy_assert(yyjson_sr_arr_next(&sr));
    yy_assert(yyjson_sr_read_sint(&sr) == INT64_MAX);
    yy_assert(yyjson_sr_arr_next(&sr));
    yy_assert(yyjson_sr_read_real(&sr) == 1.5e300);
    yy_assert(yyjson_sr_arr_next(&sr));
    yy_assert(yyjson_sr_read_real(&sr) == -2.25);
    yy_assert(yyjson_sr_arr_next(&sr));
    yy_assert(yyjson_sr_read_real(&sr) == 0.0001);
    yy_assert(yyjson_sr_arr_next(&sr));
    yy_assert(yyjson_sr_read_real(&sr) == 3e-5);
    yy_assert(yyjson_sr_arr_next(&sr));
    yy_assert(yyjson_sr_read_real(&sr) == 123456789.0); /* real over integer token */
    yy_assert(!yyjson_sr_arr_next(&sr));
    yy_assert(yyjson_sr_arr_end(&sr));
    yy_assert(yyjson_sr_ok(&sr));
    yy_assert(yyjson_sr_finish(&sr));
    free(json);

    json = copy_padded("123x", &len);
    yy_assert(init_reader(&sr, json, len));
    val = yyjson_sr_read_num(&sr);
    yy_assert(!yyjson_sr_ok(&sr));
    yy_assert(yyjson_get_type(&val) == YYJSON_TYPE_NONE);
    free(json);

    json = copy_padded("123x", &len);
    yy_assert(init_reader(&sr, json, len));
    val = yyjson_sr_read_scalar(&sr);
    yy_assert(!yyjson_sr_ok(&sr));
    yy_assert(yyjson_get_type(&val) == YYJSON_TYPE_NONE);
    free(json);
}

static void test_skip_shapes(void) {
    size_t len;
    char *json = copy_padded("[1,\"s\",true,null,{},[],{\"a\":[1,{\"b\":\"}]\\\"\"}]},"
        "[[[[1],2],3],4],-1.5e10,\"end\"]", &len);
    yyjson_sr sr;
    int n = 0;
    yyjson_sv sv = { NULL, 0 };

    yy_assert(init_reader(&sr, json, len));
    yy_assert(yyjson_sr_arr_begin(&sr));
    while (yyjson_sr_arr_next(&sr)) {
        n++;
        if (n == 10) {
            sv = yyjson_sr_read_str(&sr);
        } else {
            yy_assert(yyjson_sr_skip_value(&sr));
        }
    }
    yy_assert(yyjson_sr_arr_end(&sr));
    yy_assert(yyjson_sr_ok(&sr));
    yy_assert(n == 10);
    yy_assert(yyjson_sv_equals_str(sv, "end"));
    yy_assert(yyjson_sr_finish(&sr));
    free(json);

    json = copy_padded("{\"want\":1,\"rest1\":[{},{\"k\":\"v\"}],\"rest2\":\"tail\"}", &len);

    yy_assert(init_reader(&sr, json, len));
    yy_assert(yyjson_sr_obj_begin(&sr));
    yy_assert(yyjson_sr_obj_end(&sr)); /* skip the entire unread container */
    yy_assert(yyjson_sr_finish(&sr));

    yy_assert(init_reader(&sr, json, len));
    yy_assert(yyjson_sr_obj_begin(&sr));
    yy_assert(yyjson_sr_obj_find(&sr, "want"));
    yy_assert(yyjson_sr_read_uint(&sr) == 1);
    yy_assert(yyjson_sr_obj_end(&sr)); /* drains rest1/rest2 */
    yy_assert(yyjson_sr_finish(&sr));

    free(json);
    json = copy_padded("[1,{\"large\":[2,3,4]},5]", &len);
    yy_assert(init_reader(&sr, json, len));
    yy_assert(yyjson_sr_arr_begin(&sr));
    yy_assert(yyjson_sr_arr_next(&sr));
    yy_assert(yyjson_sr_read_uint(&sr) == 1);
    yy_assert(yyjson_sr_arr_end(&sr));
    yy_assert(yyjson_sr_finish(&sr));
    free(json);
}

static void test_peek_contract(void) {
    size_t len;
    char *json = copy_padded("[1,2]", &len);
    yyjson_sr sr;

    yy_assert(init_reader(&sr, json, len));
    yy_assert(yyjson_sr_arr_begin(&sr));
    yy_assert(yyjson_sr_arr_next(&sr));
    yy_assert(yyjson_sr_read_uint(&sr) == 1);
    yy_assert(yyjson_sr_peek_type(&sr) == YYJSON_TYPE_NONE);
    yy_assert(yyjson_sr_ok(&sr));
    yy_assert(yyjson_sr_arr_next(&sr));
    yy_assert(yyjson_sr_read_uint(&sr) == 2);
    yy_assert(yyjson_sr_arr_end(&sr));
    yy_assert(yyjson_sr_finish(&sr));

    free(json);
    json = copy_padded("[]", &len);
    yy_assert(init_reader(&sr, json, len));
    yy_assert(yyjson_sr_arr_begin(&sr));
    yy_assert(!yyjson_sr_arr_next(&sr));
    yy_assert(yyjson_sr_peek_type(&sr) == YYJSON_TYPE_NONE);
    yy_assert(yyjson_sr_ok(&sr));
    yy_assert(yyjson_sr_arr_end(&sr));
    yy_assert(yyjson_sr_finish(&sr));
    free(json);
}

static void test_ndjson(void) {
    size_t len;
    char *json = copy_padded("{\"n\":1}\n{\"n\":2}\n{\"n\":3}\n", &len);
    yyjson_sr sr;
    uint64_t sum = 0;

    yy_assert(init_reader(&sr, json, len));
    do {
        yy_assert(yyjson_sr_obj_begin(&sr));
        yy_assert(yyjson_sr_obj_find(&sr, "n"));
        sum += yyjson_sr_read_uint(&sr);
        yy_assert(yyjson_sr_obj_end(&sr));
    } while (yyjson_sr_doc_next(&sr));
    yy_assert(yyjson_sr_ok(&sr));
    yy_assert(sum == 6);

    free(json);
    json = copy_padded("{\"n\":1}\n{\"n\":2}\n{\"n\":3}\n", &len);
    sum = 0;
    yy_assert(init_reader(&sr, json, len));
    while (yyjson_sr_doc_next(&sr)) {
        yy_assert(yyjson_sr_obj_begin(&sr));
        yy_assert(yyjson_sr_obj_find(&sr, "n"));
        sum += yyjson_sr_read_uint(&sr);
        yy_assert(yyjson_sr_obj_end(&sr));
    }
    yy_assert(yyjson_sr_ok(&sr));
    yy_assert(sum == 6);

    free(json);
    json = copy_padded("{\"n\":1}\n{\"n\":2}\n{\"n\":3}\n", &len);
    yy_assert(init_reader(&sr, json, len));
    yy_assert(yyjson_sr_doc_next(&sr));
    yy_assert(!yyjson_sr_doc_next(&sr));
    yy_assert(!yyjson_sr_ok(&sr));
    free(json);
}

/* Strictly read one scalar based on its peeked type. */
static void read_current_scalar(yyjson_sr *sr) {
    switch (yyjson_sr_peek_type(sr)) {
        case YYJSON_TYPE_STR:
            (void)yyjson_sr_read_str(sr);
            break;
        case YYJSON_TYPE_BOOL:
            (void)yyjson_sr_read_bool(sr);
            break;
        case YYJSON_TYPE_NULL:
            yyjson_sr_read_null(sr);
            break;
        default:
            (void)yyjson_sr_read_real(sr);
            break;
    }
}

/* Strictly read one shallow document and require it to fail. */
static void assert_strict_error(const char *raw) {
    size_t len;
    char *json = copy_padded(raw, &len);
    yyjson_sr sr;
    yyjson_sv key;
    yyjson_read_err err;

    yy_assert(init_reader(&sr, json, len));
    switch (yyjson_sr_peek_type(&sr)) {
        case YYJSON_TYPE_OBJ:
            if (yyjson_sr_obj_begin(&sr)) {
                while (yyjson_sr_ok(&sr) && yyjson_sr_obj_next(&sr, &key)) {
                    read_current_scalar(&sr);
                }
                if (yyjson_sr_ok(&sr)) (void)yyjson_sr_obj_end(&sr);
            }
            break;
        case YYJSON_TYPE_ARR:
            if (yyjson_sr_arr_begin(&sr)) {
                while (yyjson_sr_ok(&sr) && yyjson_sr_arr_next(&sr)) {
                    read_current_scalar(&sr);
                }
                if (yyjson_sr_ok(&sr)) (void)yyjson_sr_arr_end(&sr);
            }
            break;
        default:
            read_current_scalar(&sr);
            break;
    }
    if (yyjson_sr_ok(&sr)) (void)yyjson_sr_finish(&sr);
    yy_assert(!yyjson_sr_ok(&sr));
    yy_assert(yyjson_sr_get_err(&sr, &err) != YYJSON_READ_SUCCESS);
    yy_assert(err.code != YYJSON_READ_SUCCESS);
    
    /* sticky: everything now fails fast */
    yy_assert(!yyjson_sr_obj_begin(&sr));
    yy_assert(!yyjson_sr_arr_next(&sr));
    yy_assert(yyjson_sr_read_uint(&sr) == 0);
    free(json);
}

static void test_errors(void) {
    static const char *invalid[] = {
        "{\"a\":1",         /* truncated object */
        "[1,2",             /* truncated array */
        "[1,]",             /* trailing comma */
        "{\"a\":1,}",       /* trailing comma */
        "{\"a\" 1}",        /* missing colon */
        "[\"\\q\"]",        /* invalid escape */
        "[\"\\ud800x\"]",   /* lone surrogate */
        "[\"a\nb\"]",       /* raw control character */
        "[01]",             /* leading zero */
        "[+1]",             /* invalid number */
        "[tru]",            /* invalid literal */
        "[1} ",             /* container mismatch */
        "\"a\" 1"           /* content after root */
    };
    size_t len;
    char *json = copy_padded("", &len);
    yyjson_sr sr;
    yyjson_read_err err;

    yy_assert(init_reader(&sr, json, len));
    yy_assert(!yyjson_sr_finish(&sr));
    yy_assert(yyjson_sr_get_err(&sr, &err) == YYJSON_READ_ERROR_EMPTY_CONTENT);

    yy_assert(init_reader(&sr, json, len));
    yy_assert(!yyjson_sr_doc_next(&sr));
    yy_assert(yyjson_sr_ok(&sr));
    free(json);

    for (size_t i = 0; i < yy_nelems(invalid); i++) assert_strict_error(invalid[i]);

    json = copy_padded("{\"a\":\"str\"}", &len);
    yy_assert(init_reader(&sr, json, len));
    yy_assert(yyjson_sr_obj_begin(&sr));
    yy_assert(yyjson_sr_obj_find(&sr, "a"));
    yy_assert(yyjson_sr_read_uint(&sr) == 0);
    yy_assert(!yyjson_sr_ok(&sr));
    free(json);
}

/* Malicious/garbage content in skipped regions must not crash or error:
   skipping is structural only, by design. */
static void test_relaxed_skip(void) {
    size_t len;
    char *json = copy_padded(
        "{\"junk1\":[\"\\q\\z\",01,+5,tru,-,\"a\nb\"],"
        "\"junk2\":{\"\\ud800\":lol,\"x\":9x9},"
        "\"good\":7}", &len);
    yyjson_sr sr;

    yy_assert(init_reader(&sr, json, len));
    yy_assert(yyjson_sr_obj_begin(&sr));
    yy_assert(yyjson_sr_obj_find(&sr, "good"));
    yy_assert(yyjson_sr_read_uint(&sr) == 7);
    yy_assert(yyjson_sr_obj_end(&sr));
    yy_assert(yyjson_sr_finish(&sr));
    yy_assert(yyjson_sr_ok(&sr));
    free(json);

    json = copy_padded("{\"skip1\":[{]],\"skip2\":{\"a\" 1 \"b\":2,},\"good\":9}", &len);
    yy_assert(init_reader(&sr, json, len));
    yy_assert(yyjson_sr_obj_begin(&sr));
    yy_assert(yyjson_sr_obj_find(&sr, "good"));
    yy_assert(yyjson_sr_read_uint(&sr) == 9);
    yy_assert(yyjson_sr_obj_end(&sr));
    yy_assert(yyjson_sr_finish(&sr));
    yy_assert(yyjson_sr_ok(&sr));
    free(json);
}

static void assert_skip_structure_error(const char *text) {
    size_t len;
    char *json = copy_padded(text, &len);
    yyjson_sr sr;
    yy_assert(init_reader(&sr, json, len));
    yy_assert(!yyjson_sr_skip_value(&sr) || !yyjson_sr_finish(&sr));
    yy_assert(!yyjson_sr_ok(&sr));
    free(json);
}

static void assert_relaxed_skip_ok(const char *text) {
    size_t len;
    char *json = copy_padded(text, &len);
    yyjson_sr sr;
    yy_assert(init_reader(&sr, json, len));
    yy_assert(yyjson_sr_skip_value(&sr));
    yy_assert(yyjson_sr_finish(&sr));
    yy_assert(yyjson_sr_ok(&sr));
    free(json);
}

static void test_structural_validation(void) {
    static const char *relaxed[] = {
        "[[}}",
        "[1 2]",
        "[1,]",
        "{\"a\" 1}",
        "{\"a\":1 \"b\":2}",
        "{\"a\":1,}",
        "{\"a\":[1,2}}"
    };
    static const char *invalid[] = { "[[]", "[]]" };
    size_t len;

    char *json;
    yyjson_sr sr;
    
    for (size_t i = 0; i < yy_nelems(relaxed); i++) assert_relaxed_skip_ok(relaxed[i]);
    for (size_t i = 0; i < yy_nelems(invalid); i++) assert_skip_structure_error(invalid[i]);

    json = copy_padded("[}", &len);
    yy_assert(init_reader(&sr, json, len));
    yy_assert(yyjson_sr_arr_begin(&sr));
    yy_assert(!yyjson_sr_obj_end(&sr));
    yy_assert(!yyjson_sr_ok(&sr));
    free(json);

    json = copy_padded("[}", &len);
    yy_assert(init_reader(&sr, json, len));
    yy_assert(yyjson_sr_arr_begin(&sr));
    yy_assert(!yyjson_sr_arr_end(&sr));
    yy_assert(!yyjson_sr_ok(&sr));
    free(json);

    for (size_t i = 0; i < 2; i++) {
        json = copy_padded(i ? "[1,}" : "[}", &len);
        yy_assert(init_reader(&sr, json, len));
        yy_assert(yyjson_sr_arr_begin(&sr));
        if (i) {
            yy_assert(yyjson_sr_arr_next(&sr));
            yy_assert(yyjson_sr_read_uint(&sr) == 1);
        }
        yy_assert(yyjson_sr_arr_next(&sr));
        yy_assert(yyjson_sr_ok(&sr));
        yy_assert(yyjson_sr_peek_type(&sr) == YYJSON_TYPE_NONE);
        yy_assert(!yyjson_sr_skip_value(&sr));
        yy_assert(!yyjson_sr_ok(&sr));
        yy_assert(!yyjson_sr_finish(&sr));
        free(json);
    }
}

static void test_deep_nesting(void) {
    size_t depth = TEST_DEPTH, len;
    char *raw = (char *)malloc(2 * depth + 8 + YYJSON_PADDING_SIZE);
    yyjson_sr sr;

    yy_assert(raw != NULL);
    for (size_t i = 0; i < depth; i++) raw[i] = '[';
    raw[depth] = '1';
    for (size_t i = 0; i < depth; i++) raw[depth + 1 + i] = ']';
    len = 2 * depth + 1;
    memset(raw + len, 0, YYJSON_PADDING_SIZE);

    yy_assert(init_reader(&sr, raw, len));
    yy_assert(yyjson_sr_skip_value(&sr));
    yy_assert(yyjson_sr_finish(&sr));

    depth++;
    for (size_t i = 0; i < depth; i++) raw[i] = '[';
    raw[depth] = '1';
    for (size_t i = 0; i < depth; i++) raw[depth + 1 + i] = ']';
    len = 2 * depth + 1;
    memset(raw + len, 0, YYJSON_PADDING_SIZE);

    yy_assert(init_reader(&sr, raw, len));
    yy_assert(yyjson_sr_skip_value(&sr));
    yy_assert(yyjson_sr_finish(&sr));

    yy_assert(init_reader(&sr, raw, len));
#if YYJSON_READER_DEPTH_LIMIT
    if ((size_t)YYJSON_READER_DEPTH_LIMIT <= depth) {
        yyjson_read_err err;
        for (size_t i = 0; i < (size_t)YYJSON_READER_DEPTH_LIMIT; i++) {
            yy_assert(yyjson_sr_arr_begin(&sr));
        }
        yy_assert(!yyjson_sr_arr_begin(&sr));
        yy_assert(yyjson_sr_get_err(&sr, &err) == YYJSON_READ_ERROR_DEPTH);
    } else
#endif
    {
        for (size_t i = 0; i < depth; i++) {
            yy_assert(yyjson_sr_arr_begin(&sr));
        }
        yy_assert(yyjson_sr_read_uint(&sr) == 1);
        for (size_t i = 0; i < depth; i++) {
            yy_assert(yyjson_sr_arr_end(&sr));
        }
        yy_assert(yyjson_sr_finish(&sr));
    }
    
    yy_assert(init_reader(&sr, raw, len));
    sr.max_depth = 1;
    yy_assert(yyjson_sr_arr_begin(&sr));
    yy_assert(!yyjson_sr_arr_begin(&sr));
    yy_assert(!yyjson_sr_ok(&sr));
    free(raw);
}

static void test_large_string(void) {
    size_t body = 6000, len;
    char *raw = (char *)malloc(body + 64 + YYJSON_PADDING_SIZE);
    yyjson_sr sr;
    yyjson_sv sv;
    size_t off = 0;

    yy_assert(raw != NULL);
    memcpy(raw + off, "{\"k\":\"", 6);
    off += 6;
    for (size_t i = 0; i < body; i++) raw[off++] = (char)('a' + (i % 26));
    memcpy(raw + off, "\\n\",\"t\":9}", 10);
    off += 10;
    len = off;
    memset(raw + len, 0, YYJSON_PADDING_SIZE);

    yy_assert(init_reader(&sr, raw, len));
    yy_assert(yyjson_sr_obj_begin(&sr));
    yy_assert(yyjson_sr_obj_find(&sr, "k"));
    sv = yyjson_sr_read_str(&sr);
    yy_assert(yyjson_sr_ok(&sr));
    yy_assert(sv.len == body + 1);
    yy_assert(sv.ptr[0] == 'a' && sv.ptr[body] == '\n');
    assert_sv_terminated(sv);
    yy_assert(yyjson_sr_obj_find(&sr, "t"));
    yy_assert(yyjson_sr_read_uint(&sr) == 9);
    yy_assert(yyjson_sr_obj_end(&sr));
    yy_assert(yyjson_sr_finish(&sr));
    free(raw);
}

static void test_scratch_limit(void) {
    size_t body = 6000, len, off = 0;
    char *raw = (char *)malloc(body + 16 + YYJSON_PADDING_SIZE);
    unsigned char buf[YYJSON_SR_MIN_BUF];
    yyjson_sr sr;
    yyjson_read_err err;

    yy_assert(raw != NULL);
    if (g_read_mode.kind != READ_INPUT_MEMORY) {
        free(raw);
        return;
    }

    memcpy(raw + off, "[\"", 2);
    off += 2;
    for (size_t i = 0; i < body; i++) raw[off++] = 'x';
    memcpy(raw + off, "\"]", 2);
    off += 2;
    len = off;
    memset(raw + len, 0, YYJSON_PADDING_SIZE);

    yy_assert(yyjson_sr_init_mem(&sr, raw, len, buf, sizeof(buf), 0));
    yy_assert(yyjson_sr_arr_begin(&sr));
    yy_assert(yyjson_sr_arr_next(&sr));
    (void)yyjson_sr_read_str(&sr);
    yy_assert(!yyjson_sr_ok(&sr)); /* exceeds cap -> clean error, no crash */
    yy_assert(yyjson_sr_get_err(&sr, &err) == YYJSON_READ_ERROR_BUFFER_LIMIT);
    yy_assert(err.code == YYJSON_READ_ERROR_BUFFER_LIMIT);

    yy_assert(yyjson_sr_init_mem(&sr, raw, len, buf, sizeof(buf), 0));
    yy_assert(yyjson_sr_arr_begin(&sr));
    yy_assert(yyjson_sr_arr_next(&sr));
    (void)yyjson_sr_read_raw(&sr);
    yy_assert(yyjson_sr_get_err(&sr, &err) == YYJSON_READ_ERROR_BUFFER_LIMIT);

    yy_assert(yyjson_sr_init_mem(&sr, raw, len, buf, sizeof(buf), 0));
    yy_assert(yyjson_sr_arr_begin(&sr));
    yy_assert(yyjson_sr_arr_next(&sr));
    yy_assert(yyjson_sr_skip_value(&sr));
    yy_assert(!yyjson_sr_arr_next(&sr));
    yy_assert(yyjson_sr_arr_end(&sr));
    yy_assert(yyjson_sr_ok(&sr));
    yy_assert(yyjson_sr_finish(&sr));
    free(raw);
}

static void test_whitespace_everywhere(void) {
    size_t len;
    char *json = copy_padded(
        " \n\t {  \"a\" \r\n :  [ 1 , \n 2 ]  , \"b\" : { \"c\" : \"d\" } } \n ",
        &len);
    yyjson_sr sr;

    yy_assert(init_reader(&sr, json, len));
    yy_assert(yyjson_sr_obj_begin(&sr));
    yy_assert(yyjson_sr_obj_find(&sr, "a"));
    yy_assert(yyjson_sr_arr_begin(&sr));
    yy_assert(yyjson_sr_arr_next(&sr));
    yy_assert(yyjson_sr_read_uint(&sr) == 1);
    yy_assert(yyjson_sr_arr_next(&sr));
    yy_assert(yyjson_sr_read_uint(&sr) == 2);
    yy_assert(!yyjson_sr_arr_next(&sr));
    yy_assert(yyjson_sr_arr_end(&sr));
    yy_assert(yyjson_sr_obj_find(&sr, "b"));
    yy_assert(yyjson_sr_obj_begin(&sr));
    yy_assert(yyjson_sr_obj_find(&sr, "c"));
    yy_assert(yyjson_sv_equals_str(yyjson_sr_read_str(&sr), "d"));
    yy_assert(yyjson_sr_obj_end(&sr));
    yy_assert(yyjson_sr_obj_end(&sr));
    yy_assert(yyjson_sr_finish(&sr));
    free(json);
}

/* A navigation call must be followed by exactly one consumer. Navigating again
   while the value is still unread used to silently re-expose it, which shifted
   every later read by one element without ever setting an error. */
static void test_unconsumed_value(void) {
    static const char msg[] = "previous value was not consumed";
    yyjson_read_err err;
    yyjson_sr sr;
    yyjson_sv key;
    size_t len;
    char *json;

#define ASSERT_UNCONSUMED()                                 \
    do {                                                    \
        yy_assert(!yyjson_sr_ok(&sr));                      \
        yy_assert(yyjson_sr_get_err(&sr, &err) ==           \
                  YYJSON_READ_ERROR_JSON_STRUCTURE);        \
        yy_assert(strcmp(err.msg, msg) == 0);               \
    } while (0)

    /* arr_next twice, at the first element */
    json = copy_padded("[11,22,33]", &len);
    yy_assert(init_reader(&sr, json, len));
    yy_assert(yyjson_sr_arr_begin(&sr));
    yy_assert(yyjson_sr_arr_next(&sr));
    yy_assert(!yyjson_sr_arr_next(&sr));
    ASSERT_UNCONSUMED();
    free(json);

    /* and one element further in, where a comma is expected first */
    json = copy_padded("[11,22,33]", &len);
    yy_assert(init_reader(&sr, json, len));
    yy_assert(yyjson_sr_arr_begin(&sr));
    yy_assert(yyjson_sr_arr_next(&sr));
    yy_assert(yyjson_sr_read_uint(&sr) == 11);
    yy_assert(yyjson_sr_arr_next(&sr));
    yy_assert(!yyjson_sr_arr_next(&sr));
    ASSERT_UNCONSUMED();
    free(json);

    /* obj_next twice */
    json = copy_padded("{\"a\":\"x\",\"b\":\"y\"}", &len);
    yy_assert(init_reader(&sr, json, len));
    yy_assert(yyjson_sr_obj_begin(&sr));
    yy_assert(yyjson_sr_obj_next(&sr, &key));
    yy_assert(!yyjson_sr_obj_next(&sr, &key));
    ASSERT_UNCONSUMED();
    free(json);

    /* obj_find twice: this used to report a clean miss for a key that is
       present, which is indistinguishable from a legitimate absence */
    json = copy_padded("{\"a\":11,\"b\":22}", &len);
    yy_assert(init_reader(&sr, json, len));
    yy_assert(yyjson_sr_obj_begin(&sr));
    yy_assert(yyjson_sr_obj_find(&sr, "a"));
    yy_assert(!yyjson_sr_obj_find(&sr, "b"));
    ASSERT_UNCONSUMED();
    free(json);

    /* the required-field helpers take the ordered fast path */
    json = copy_padded("{\"a\":11,\"b\":22}", &len);
    yy_assert(init_reader(&sr, json, len));
    yy_assert(yyjson_sr_obj_begin(&sr));
    yy_assert(yyjson_sr_obj_find(&sr, "a"));
    yy_assert(yyjson_sr_obj_read_uint(&sr, "b") == 0);
    ASSERT_UNCONSUMED();
    free(json);

    /* --- and the cases that must not be reported --- */

    /* closing a container skips what is left, including an exposed value */
    json = copy_padded("[11,22,33]", &len);
    yy_assert(init_reader(&sr, json, len));
    yy_assert(yyjson_sr_arr_begin(&sr));
    yy_assert(yyjson_sr_arr_next(&sr));
    yy_assert(yyjson_sr_arr_end(&sr));
    yy_assert(yyjson_sr_finish(&sr));
    free(json);

    json = copy_padded("{\"a\":11,\"b\":22}", &len);
    yy_assert(init_reader(&sr, json, len));
    yy_assert(yyjson_sr_obj_begin(&sr));
    yy_assert(yyjson_sr_obj_next(&sr, &key));
    yy_assert(yyjson_sr_obj_end(&sr));
    yy_assert(yyjson_sr_finish(&sr));
    free(json);

    /* a genuine miss leaves the slot empty, so scanning again stays clean */
    json = copy_padded("{\"a\":11}", &len);
    yy_assert(init_reader(&sr, json, len));
    yy_assert(yyjson_sr_obj_begin(&sr));
    yy_assert(!yyjson_sr_obj_find(&sr, "zz"));
    yy_assert(!yyjson_sr_obj_find(&sr, "yy"));
    yy_assert(yyjson_sr_ok(&sr));
    yy_assert(yyjson_sr_obj_end(&sr));
    yy_assert(yyjson_sr_finish(&sr));
    free(json);

    /* peeking is not consuming, and repeating it must not look like one */
    json = copy_padded("[11,22]", &len);
    yy_assert(init_reader(&sr, json, len));
    yy_assert(yyjson_sr_arr_begin(&sr));
    yy_assert(yyjson_sr_arr_next(&sr));
    yy_assert(yyjson_sr_peek_type(&sr) == YYJSON_TYPE_NUM);
    yy_assert(yyjson_sr_peek_type(&sr) == YYJSON_TYPE_NUM);
    yy_assert(yyjson_sr_read_uint(&sr) == 11);
    yy_assert(yyjson_sr_arr_next(&sr));
    yy_assert(yyjson_sr_skip_value(&sr));
    yy_assert(!yyjson_sr_arr_next(&sr));
    yy_assert(yyjson_sr_arr_end(&sr));
    yy_assert(yyjson_sr_finish(&sr));
    free(json);

    /* entering a container consumes it; leaving one marks it consumed */
    json = copy_padded("[[1],[2],{\"k\":3}]", &len);
    yy_assert(init_reader(&sr, json, len));
    yy_assert(yyjson_sr_arr_begin(&sr));
    yy_assert(yyjson_sr_arr_next(&sr));
    yy_assert(yyjson_sr_arr_begin(&sr));
    yy_assert(yyjson_sr_arr_next(&sr));
    yy_assert(yyjson_sr_read_uint(&sr) == 1);
    yy_assert(!yyjson_sr_arr_next(&sr));
    yy_assert(yyjson_sr_arr_end(&sr));
    yy_assert(yyjson_sr_arr_next(&sr));
    yy_assert(yyjson_sr_skip_value(&sr));
    yy_assert(yyjson_sr_arr_next(&sr));
    yy_assert(yyjson_sr_obj_begin(&sr));
    yy_assert(yyjson_sr_obj_read_uint(&sr, "k") == 3);
    yy_assert(yyjson_sr_obj_end(&sr));
    yy_assert(!yyjson_sr_arr_next(&sr));
    yy_assert(yyjson_sr_arr_end(&sr));
    yy_assert(yyjson_sr_finish(&sr));
    free(json);

#undef ASSERT_UNCONSUMED
}

#define POS_ITEMS 400
#define POS_STRIDE 15 /* `"aaaaaaaaaaaa"` plus one separator */

/* Fills `json` with `[ "aaaaaaaaaaaa", ... ]` and returns its length. The
   insitu reader rewrites the buffer, so each pass needs a fresh copy. */
static size_t pos_build(char *json) {
    size_t off = 0;
    json[off++] = '[';
    for (size_t i = 0; i < POS_ITEMS; i++) {
        if (i) json[off++] = ',';
        json[off++] = '"';
        memset(json + off, 'a', 12);
        off += 12;
        json[off++] = '"';
    }
    json[off++] = ']';
    memset(json + off, 0, YYJSON_PADDING_SIZE);
    return off;
}

static void test_reader_pos(void) {
    size_t len, bad;
    char *json = (char *)malloc(POS_ITEMS * POS_STRIDE + 8 + YYJSON_PADDING_SIZE);
    yyjson_sr sr;
    yyjson_read_err err;

    yy_assert(json != NULL);
    len = pos_build(json);
    yy_assert(init_reader(&sr, json, len));
    yy_assert(yyjson_sr_pos(&sr) == 0);
    yy_assert(yyjson_sr_arr_begin(&sr));
    yy_assert(yyjson_sr_pos(&sr) == 1);
    for (size_t i = 0; i < POS_ITEMS; i++) {
        yy_assert(yyjson_sr_arr_next(&sr));
        /* positioned at the opening quote of element i */
        yy_assert(yyjson_sr_pos(&sr) == 1 + i * POS_STRIDE);
        yy_assert(yyjson_sr_read_str(&sr).len == 12);
        /* positioned just past its closing quote */
        yy_assert(yyjson_sr_pos(&sr) == POS_STRIDE * (i + 1));
    }
    yy_assert(!yyjson_sr_arr_next(&sr));
    yy_assert(yyjson_sr_arr_end(&sr));
    yy_assert(yyjson_sr_finish(&sr));
    yy_assert(yyjson_sr_pos(&sr) == len);

    len = pos_build(json);
    bad = (POS_ITEMS - 1) * POS_STRIDE;
    json[bad] = '#';
    yy_assert(init_reader(&sr, json, len));
    yy_assert(yyjson_sr_arr_begin(&sr));
    for (size_t i = 0; i < POS_ITEMS; i++) {
        if (!yyjson_sr_arr_next(&sr)) break;
        if (!yyjson_sr_read_str(&sr).ptr) break;
    }
    yy_assert(!yyjson_sr_ok(&sr));
    yy_assert(yyjson_sr_get_err(&sr, &err) != 0);
    yy_assert(err.pos == bad);
    free(json);
}

#undef POS_ITEMS
#undef POS_STRIDE


/*==============================================================================
 * MARK: - Reader Initialization and Buffer Limits
 *============================================================================*/

static void test_memory_input_reuse(void) {
    static const char json[] = "{\"a\":\"x\\ny\",\"b\":\"plain\"}";
    static const char scalar[] = "42";
    char original[sizeof(json)];
    unsigned char buf[YYJSON_SR_MIN_BUF];
    yyjson_sr sr;
    yyjson_sv sv;

    memcpy(original, json, sizeof(json));
    yy_assert(yyjson_sr_init_mem(&sr, (char *)json, sizeof(json) - 1, buf, sizeof(buf), 0));
    yy_assert(yyjson_sr_obj_begin(&sr));
    yy_assert(yyjson_sr_obj_find(&sr, "a"));
    sv = yyjson_sr_read_str(&sr);
    yy_assert(sv.ptr && sv.len == 3 && sv.ptr[0] == 'x' && sv.ptr[1] == '\n' &&
              sv.ptr[2] == 'y');
    assert_sv_terminated(sv);
    yy_assert(yyjson_sr_obj_find(&sr, "b"));
    sv = yyjson_sr_read_str(&sr);
    yy_assert(yyjson_sv_equals_str(sv, "plain"));
    assert_sv_terminated(sv);
    yy_assert(yyjson_sr_obj_end(&sr));
    yy_assert(yyjson_sr_finish(&sr));
    yy_assert(memcmp(json, original, sizeof(json)) == 0);

    yy_assert(yyjson_sr_init_mem(&sr, (char *)scalar, sizeof(scalar) - 1, buf, sizeof(buf), 0));
    yy_assert(yyjson_sr_read_uint(&sr) == 42);
    yy_assert(yyjson_sr_finish(&sr));
}

static void test_memory_insitu(void) {
    static const char source[] = "{\"s\":\"a\\nb\"}";
    char json[sizeof(source) + YYJSON_PADDING_SIZE];
    unsigned char buf[YYJSON_SR_MIN_BUF];
    yyjson_sr sr;
    yyjson_sv value;

    memcpy(json, source, sizeof(source) - 1);
    memset(json + sizeof(source) - 1, 0, YYJSON_PADDING_SIZE);
    yy_assert(yyjson_sr_init_mem(&sr, json, sizeof(source) - 1, buf, sizeof(buf), YYJSON_READ_INSITU));
    yy_assert(yyjson_sr_obj_begin(&sr));
    yy_assert(yyjson_sr_obj_find(&sr, "s"));
    value = yyjson_sr_read_str(&sr);
    yy_assert(value.ptr >= json && value.ptr < json + sizeof(source));
    yy_assert(value.len == 3 && memcmp(value.ptr, "a\nb", 3) == 0);
    assert_sv_terminated(value);
    yy_assert(yyjson_sr_obj_end(&sr));
    yy_assert(yyjson_sr_finish(&sr));
    yy_assert(memcmp(json, source, sizeof(source) - 1) != 0);
}

static void test_reader_flags(void) {
    unsigned char buf[YYJSON_SR_MIN_BUF];
    yyjson_sr sr;
    yyjson_read_err err;

    yy_assert(yyjson_sr_init_mem(&sr, "1", 1, buf, sizeof(buf),
                                 YYJSON_READ_ALLOW_INF_AND_NAN |
                                 YYJSON_READ_ALLOW_COMMENTS));
    yy_assert(yyjson_sr_read_uint(&sr) == 1);
    yy_assert(yyjson_sr_finish(&sr));

    yy_assert(yyjson_sr_init_mem(&sr, "NaN", 3, buf, sizeof(buf),
                                 YYJSON_READ_ALLOW_INF_AND_NAN));
    (void)yyjson_sr_read_real(&sr);
    yy_assert(yyjson_sr_get_err(&sr, &err) != YYJSON_READ_SUCCESS);

    /* comments stay rejected even with the flag set */
    yy_assert(yyjson_sr_init_mem(&sr, "[/*c*/1]", 8, buf, sizeof(buf),
                                 YYJSON_READ_ALLOW_COMMENTS));
    (void)yyjson_sr_arr_begin(&sr);
    (void)yyjson_sr_arr_next(&sr);
    (void)yyjson_sr_read_uint(&sr);
    yy_assert(yyjson_sr_get_err(&sr, &err) != YYJSON_READ_SUCCESS);
}

static size_t fail_read(void *ctx, void *dst, size_t cap) {
    (void)ctx;
    (void)dst;
    (void)cap;
    return ((size_t)-1);
}

static void test_reader_buf(void) {
    unsigned char too_small[YYJSON_SR_MIN_BUF - 1];
    unsigned char minimum[YYJSON_SR_MIN_BUF];
    unsigned char unaligned[YYJSON_SR_MIN_BUF + 1];
    unsigned char *buf;
    char *token;
    size_t token_len = TEST_SR_MIN_WINDOW - YYJSON_PADDING_SIZE;
    yyjson_sr sr;
    yyjson_read_err err;
    yyjson_sv sv;

    yy_assert(!yyjson_sr_init_mem(&sr, "0", 1, too_small, sizeof(too_small), 0));
    yy_assert(yyjson_sr_init_mem(&sr, "0", 1, minimum, sizeof(minimum), 0));
    yy_assert(sr.win_cap == TEST_SR_MIN_WINDOW);
    yy_assert((size_t)(sr.stk - sr.tmp) >= YYJSON_SR_MIN_BUF - sr.win_cap);

    yy_assert(yyjson_sr_init_mem(&sr, "0", 1, unaligned + 1, YYJSON_SR_MIN_BUF, 0));
    yy_assert(yyjson_sr_read_uint(&sr) == 0);
    yy_assert(yyjson_sr_finish(&sr));

    /* the minimum buffer still supports a token spanning two windows */
    token = (char *)malloc(token_len + 3 + YYJSON_PADDING_SIZE);
    yy_assert(token != NULL);
    token[0] = '"';
    memset(token + 1, 'x', token_len);
    token[token_len + 1] = '"';
    memset(token + token_len + 2, 0, YYJSON_PADDING_SIZE);
    yy_assert(yyjson_sr_init_mem(&sr, token, token_len + 2, minimum, sizeof(minimum), 0));
    sv = yyjson_sr_read_str(&sr);
    yy_assert(sv.len == token_len);
    assert_sv_terminated(sv);
    yy_assert(yyjson_sr_finish(&sr));

    /* one more retained byte exceeds the minimum buffer's guarantee */
    token[token_len + 1] = 'x';
    token[token_len + 2] = '"';
    memset(token + token_len + 3, 0, YYJSON_PADDING_SIZE);
    yy_assert(yyjson_sr_init_mem(&sr, token, token_len + 3, minimum, sizeof(minimum), 0));
    (void)yyjson_sr_read_str(&sr);
    yy_assert(yyjson_sr_get_err(&sr, &err) == YYJSON_READ_ERROR_BUFFER_LIMIT);
    free(token);

    buf = (unsigned char *)malloc(YYJSON_SR_DEF_BUF);
    yy_assert(buf != NULL);
    yy_assert(yyjson_sr_init_mem(&sr, "0", 1, buf, YYJSON_SR_DEF_BUF, 0));
    yy_assert(sr.win_cap == TEST_SR_RECOMMENDED_WINDOW);
    yy_assert((size_t)(sr.stk - sr.tmp) == YYJSON_SR_DEF_BUF - TEST_SR_RECOMMENDED_WINDOW);
    free(buf);

    buf = (unsigned char *)malloc(TEST_SR_MAX_WINDOW * 2);
    yy_assert(buf != NULL);
    yy_assert(yyjson_sr_init_mem(&sr, "0", 1, buf, TEST_SR_MAX_WINDOW * 2, 0));
    yy_assert(sr.win_cap == TEST_SR_MAX_WINDOW);
    yy_assert((size_t)(sr.stk - sr.tmp) == TEST_SR_MAX_WINDOW);
    free(buf);

    yy_assert(yyjson_sr_init_fn(&sr, fail_read, NULL, minimum, sizeof(minimum), 0));
    (void)yyjson_sr_peek_type(&sr);
    yy_assert(yyjson_sr_get_err(&sr, &err) == YYJSON_READ_ERROR_IO);
    yy_assert(err.code == YYJSON_READ_ERROR_IO);
}

static void test_reader_buf_size(void) {
    enum { token_len = 70000, depth = 4 };
    static unsigned char buf[YYJSON_SR_BUF_SIZE(token_len, depth)];
    unsigned char insitu_stk[depth];
    unsigned char short_stk[depth - 1];
    char *json = (char *)malloc(token_len + depth * 2 + YYJSON_PADDING_SIZE);
    read_feed feed;
    yyjson_sr sr;
    size_t len = 0;

    yy_assert(json != NULL);
    for (size_t i = 0; i < depth; i++) json[len++] = '[';
    json[len] = '0';
    json[len + 1] = '.';
    for (size_t i = 2; i < token_len; i++) {
        json[len + i] = (char)('1' + i % 9);
    }
    len += token_len;
    for (size_t i = 0; i < depth; i++) json[len++] = ']';
    memset(json + len, 0, YYJSON_PADDING_SIZE);

    feed.dat = json;
    feed.len = len;
    feed.pos = 0;
    feed.step = 17;
    yy_assert(yyjson_sr_init_fn(&sr, read_feed_fn, &feed, buf, sizeof(buf), 0));
    for (size_t i = 0; i < depth; i++) {
        yy_assert(yyjson_sr_arr_begin(&sr));
    }
    (void)yyjson_sr_read_real(&sr);
    for (size_t i = 0; i < depth; i++) {
        yy_assert(yyjson_sr_arr_end(&sr));
    }
    yy_assert(yyjson_sr_finish(&sr));

    /* insitu uses `buf_cap` directly as its explicit depth capacity */
    yy_assert(yyjson_sr_init_mem(&sr, json, len, insitu_stk,
                                 sizeof(insitu_stk), YYJSON_READ_INSITU));
    for (size_t i = 0; i < depth; i++) {
        yy_assert(yyjson_sr_arr_begin(&sr));
    }
    (void)yyjson_sr_read_real(&sr);
    for (size_t i = 0; i < depth; i++) {
        yy_assert(yyjson_sr_arr_end(&sr));
    }
    yy_assert(yyjson_sr_finish(&sr));

    yy_assert(yyjson_sr_init_mem(&sr, json, len, short_stk,
                                 sizeof(short_stk), YYJSON_READ_INSITU));
    for (size_t i = 0; i < depth - 1; i++) {
        yy_assert(yyjson_sr_arr_begin(&sr));
    }
    yy_assert(!yyjson_sr_arr_begin(&sr));
    yy_assert(!yyjson_sr_ok(&sr));
    free(json);
}

static void fill_long_real(char *dat, size_t len) {

    yy_assert(len >= 3);
    dat[0] = '0';
    dat[1] = '.';
    for (size_t i = 2; i < len; i++) dat[i] = (char)('1' + i % 9);
}

static void test_reader_window_boundary(void) {
    const size_t normal_cap = TEST_SR_MIN_WINDOW - YYJSON_PADDING_SIZE;
    const size_t key_len = normal_cap - 3;
    unsigned char buf[YYJSON_SR_MIN_BUF];
    char *json = (char *)malloc(normal_cap + 16);
    char *key = (char *)malloc(key_len + 1);
    yyjson_sr sr;
    yyjson_sv sv;
    yyjson_read_err err;
    size_t len;

    yy_assert(json && key);

    /* a number filling the normal window can end at EOF */
    fill_long_real(json, normal_cap);
    yy_assert(yyjson_sr_init_mem(&sr, json, normal_cap, buf, sizeof(buf), 0));
    (void)yyjson_sr_read_real(&sr);
    yy_assert(yyjson_sr_ok(&sr));
    yy_assert(yyjson_sr_finish(&sr));

    /* a near-full number leaves one window byte for the delimiter */
    json[0] = '[';
    fill_long_real(json + 1, normal_cap - 1);
    memcpy(json + normal_cap, ",0]", 3);
    len = normal_cap + 3;
    yy_assert(yyjson_sr_init_mem(&sr, json, len, buf, sizeof(buf), 0));
    yy_assert(yyjson_sr_arr_begin(&sr));
    yy_assert(yyjson_sr_arr_next(&sr));
    (void)yyjson_sr_read_real(&sr);
    yy_assert(yyjson_sr_arr_next(&sr));
    yy_assert(yyjson_sr_read_uint(&sr) == 0);
    yy_assert(!yyjson_sr_arr_next(&sr));
    yy_assert(yyjson_sr_arr_end(&sr));
    yy_assert(yyjson_sr_finish(&sr));

    /* a full-window number with more input cannot fit */
    json[0] = '[';
    fill_long_real(json + 1, normal_cap);
    memcpy(json + 1 + normal_cap, ",0]", 3);
    len = normal_cap + 4;
    yy_assert(yyjson_sr_init_mem(&sr, json, len, buf, sizeof(buf), 0));
    yy_assert(yyjson_sr_arr_begin(&sr));
    yy_assert(yyjson_sr_arr_next(&sr));
    (void)yyjson_sr_read_real(&sr);
    yy_assert(yyjson_sr_get_err(&sr, &err) == YYJSON_READ_ERROR_BUFFER_LIMIT);
    yy_assert(err.code == YYJSON_READ_ERROR_BUFFER_LIMIT);

    /* one more number byte exceeds the explicit window token limit */
    fill_long_real(json, normal_cap + 1);
    yy_assert(yyjson_sr_init_mem(&sr, json, normal_cap + 1, buf, sizeof(buf), 0));
    (void)yyjson_sr_read_real(&sr);
    yy_assert(yyjson_sr_get_err(&sr, &err) == YYJSON_READ_ERROR_BUFFER_LIMIT);
    yy_assert(err.code == YYJSON_READ_ERROR_BUFFER_LIMIT);

    /* a boundary key is copied to the scratch area only when needed */
    for (size_t i = 0; i < key_len; i++) key[i] = (char)('a' + i % 26);
    key[key_len] = '\0';
    json[0] = '{';
    json[1] = '"';
    memcpy(json + 2, key, key_len);
    memcpy(json + 2 + key_len, "\":1}", 4);
    len = key_len + 6;
    yy_assert(yyjson_sr_init_mem(&sr, json, len, buf, sizeof(buf), 0));
    yy_assert(yyjson_sr_obj_begin(&sr));
    yy_assert(yyjson_sr_obj_next(&sr, &sv));
    yy_assert(sv.len == key_len && memcmp(sv.ptr, key, key_len) == 0);
    yy_assert(yyjson_sr_read_uint(&sr) == 1);
    yy_assert(!yyjson_sr_obj_next(&sr, &sv));
    yy_assert(yyjson_sr_obj_end(&sr));
    yy_assert(yyjson_sr_finish(&sr));

    yy_assert(yyjson_sr_init_mem(&sr, json, len, buf, sizeof(buf), 0));
    yy_assert(yyjson_sr_obj_begin(&sr));
    yy_assert(yyjson_sr_obj_findn(&sr, key, key_len));
    yy_assert(yyjson_sr_read_uint(&sr) == 1);
    yy_assert(yyjson_sr_obj_end(&sr));
    yy_assert(yyjson_sr_finish(&sr));

    free(key);
    free(json);
}

static void test_reader_null_inputs(void) {
    unsigned char sr_buf[YYJSON_SR_MIN_BUF];
    yyjson_sr sr;
    yyjson_sv sv;
    yyjson_val val;

    yy_assert(!yyjson_sr_init_mem(NULL, "0", 1, sr_buf, sizeof(sr_buf), 0));
    yy_assert(!yyjson_sr_init_mem(&sr, NULL, 0, sr_buf, sizeof(sr_buf), 0));
    yy_assert(!yyjson_sr_init_mem(&sr, "0", (size_t)-1, sr_buf, sizeof(sr_buf), 0));
    /* a failed init leaves a sticky error; later public calls stay safe */
    yy_assert(!yyjson_sr_arr_begin(&sr));
    yy_assert(!yyjson_sr_obj_begin(&sr));
    yy_assert(!yyjson_sr_arr_end(&sr));
    yy_assert(!yyjson_sr_obj_end(&sr));
    yy_assert(!yyjson_sr_init_fn(NULL, read_feed_fn, NULL, sr_buf, sizeof(sr_buf), 0));
    yy_assert(!yyjson_sr_init_fn(&sr, NULL, NULL, sr_buf, sizeof(sr_buf), 0));
    yy_assert(!yyjson_sr_init_fn(&sr, read_feed_fn, NULL, NULL, sizeof(sr_buf), 0));
    yy_assert(!yyjson_sr_ok(NULL));
    yy_assert(yyjson_sr_get_err(NULL, NULL) == YYJSON_READ_ERROR_INVALID_PARAMETER);
    yy_assert(yyjson_sr_init_mem(&sr, "0", 1, sr_buf, sizeof(sr_buf), 0));
    yy_assert(yyjson_sr_get_err(&sr, NULL) == YYJSON_READ_SUCCESS);
    yy_assert(yyjson_sr_pos(NULL) == 0);
    yy_assert(yyjson_sr_peek_type(NULL) == YYJSON_TYPE_NONE);
    yy_assert(!yyjson_sr_obj_begin(NULL));
    yy_assert(!yyjson_sr_arr_begin(NULL));
    yy_assert(!yyjson_sr_obj_next(NULL, &sv));
    yy_assert(!yyjson_sr_obj_next(NULL, NULL));
    yy_assert(!yyjson_sr_obj_find(NULL, "key"));
    yy_assert(!yyjson_sr_obj_find(NULL, NULL));
    yy_assert(!yyjson_sr_obj_findn(NULL, "key", 3));
    yy_assert(!yyjson_sr_obj_findn(NULL, NULL, 0));
    yy_assert(yyjson_sr_obj_read_real(NULL, "key") == 0.0);
    yy_assert(yyjson_sr_obj_read_uint(NULL, "key") == 0);
    yy_assert(yyjson_sr_obj_read_sint(NULL, "key") == 0);
    sv = yyjson_sr_obj_read_str(NULL, "key");
    yy_assert(!sv.ptr && sv.len == 0);
    yy_assert(!yyjson_sr_obj_read_bool(NULL, "key"));
    yyjson_sr_obj_read_null(NULL, "key");
    yy_assert(!yyjson_sr_obj_end(NULL));
    yy_assert(!yyjson_sr_arr_end(NULL));
    yy_assert(!yyjson_sr_arr_next(NULL));
    sv = yyjson_sr_read_str(NULL);
    yy_assert(!sv.ptr && sv.len == 0);
    sv = yyjson_sr_read_raw(NULL);
    yy_assert(!sv.ptr && sv.len == 0);
    val = yyjson_sr_read_num(NULL);
    yy_assert(yyjson_get_type(&val) == YYJSON_TYPE_NONE);
    val = yyjson_sr_read_scalar(NULL);
    yy_assert(yyjson_get_type(&val) == YYJSON_TYPE_NONE);
    yy_assert(yyjson_sr_read_uint(NULL) == 0);
    yy_assert(yyjson_sr_read_sint(NULL) == 0);
    yy_assert(yyjson_sr_read_real(NULL) == 0.0);
    yy_assert(!yyjson_sr_read_bool(NULL));
    yyjson_sr_read_null(NULL);
    yy_assert(!yyjson_sr_skip_value(NULL));
    yy_assert(!yyjson_sr_finish(NULL));
    yy_assert(!yyjson_sr_doc_next(NULL));

    /* required secondary pointers are checked before they are used */
    yy_assert(yyjson_sr_init_mem(&sr, "{}", 2, sr_buf, sizeof(sr_buf), 0));
    yy_assert(yyjson_sr_obj_begin(&sr));
    yy_assert(!yyjson_sr_obj_next(&sr, NULL));
    yy_assert(!yyjson_sr_ok(&sr));
    yy_assert(yyjson_sr_init_mem(&sr, "{}", 2, sr_buf, sizeof(sr_buf), 0));
    yy_assert(yyjson_sr_obj_begin(&sr));
    yy_assert(!yyjson_sr_obj_find(&sr, NULL));
    yy_assert(!yyjson_sr_ok(&sr));
    yy_assert(yyjson_sr_init_mem(&sr, "{}", 2, sr_buf, sizeof(sr_buf), 0));
    yy_assert(yyjson_sr_obj_begin(&sr));
    yy_assert(yyjson_sr_obj_read_uint(&sr, NULL) == 0);
    yy_assert(!yyjson_sr_ok(&sr));
    yy_assert(yyjson_sr_init_mem(&sr, "{}", 2, sr_buf, sizeof(sr_buf), 0));
    yy_assert(yyjson_sr_obj_begin(&sr));
    sv = yyjson_sr_obj_read_str(&sr, NULL);
    yy_assert(!sv.ptr && sv.len == 0);
    yy_assert(!yyjson_sr_ok(&sr));
    yy_assert(yyjson_sr_init_mem(&sr, "{}", 2, sr_buf, sizeof(sr_buf), 0));
    yy_assert(yyjson_sr_obj_begin(&sr));
    yy_assert(!yyjson_sr_obj_read_bool(&sr, NULL));
    yy_assert(!yyjson_sr_ok(&sr));
    yy_assert(yyjson_sr_init_mem(&sr, "{}", 2, sr_buf, sizeof(sr_buf), 0));
    yy_assert(yyjson_sr_obj_begin(&sr));
    yyjson_sr_obj_read_null(&sr, NULL);
    yy_assert(!yyjson_sr_ok(&sr));
}

#endif /* !YYJSON_DISABLE_STREAMING && !YYJSON_DISABLE_READER */


/*==============================================================================
 * MARK: - Writer
 *============================================================================*/

#if !YYJSON_DISABLE_STREAMING && !YYJSON_DISABLE_WRITER

static void test_writer_null_inputs(void) {
    unsigned char sw_buf[YYJSON_SW_MIN_BUF];
    yyjson_sw sw;
    yyjson_sv sv;
    yyjson_val val = {0};

    /* public writer APIs accept NULL defensively; unsafe_ APIs do not */
    yy_assert(!yyjson_sw_init_mem(NULL, sw_buf, sizeof(sw_buf), 0));
    yy_assert(!yyjson_sw_init_mem(&sw, NULL, sizeof(sw_buf), 0));
    yy_assert(!yyjson_sw_init_fn(NULL, NULL, NULL, sw_buf, sizeof(sw_buf), 0));
    yy_assert(!yyjson_sw_init_fn(&sw, NULL, NULL, sw_buf, sizeof(sw_buf), 0));
    yy_assert(!yyjson_sw_flush(NULL));
    yy_assert(!yyjson_sw_finish(NULL));
    yy_assert(!yyjson_sw_ok(NULL));
    yy_assert(yyjson_sw_get_err(NULL, NULL) == YYJSON_WRITE_ERROR_INVALID_PARAMETER);
    yy_assert(yyjson_sw_init_mem(&sw, sw_buf, sizeof(sw_buf), 0));
    yy_assert(yyjson_sw_get_err(&sw, NULL) == YYJSON_WRITE_SUCCESS);
    yy_assert(yyjson_sw_len(NULL) == 0);
    yy_assert(!yyjson_sw_obj_begin(NULL));
    yy_assert(!yyjson_sw_obj_end(NULL));
    yy_assert(!yyjson_sw_arr_begin(NULL));
    yy_assert(!yyjson_sw_arr_end(NULL));
    yy_assert(!yyjson_sw_write_key(NULL, "key"));
    yy_assert(!yyjson_sw_write_keyn(NULL, "key", 3));
    yy_assert(!yyjson_sw_write_str(NULL, "str"));
    yy_assert(!yyjson_sw_write_strn(NULL, "str", 3));
    sv.ptr = "str";
    sv.len = 3;
    yy_assert(!yyjson_sw_write_key_sv(NULL, sv));
    yy_assert(!yyjson_sw_write_sv(NULL, sv));
    yy_assert(!yyjson_sw_write_uint(NULL, 0));
    yy_assert(!yyjson_sw_write_sint(NULL, 0));
    yy_assert(!yyjson_sw_write_real(NULL, 0.0));
    yy_assert(!yyjson_sw_write_num(NULL, &val));
    yy_assert(!yyjson_sw_write_scalar(NULL, &val));
    yy_assert(!yyjson_sw_write_bool(NULL, false));
    yy_assert(!yyjson_sw_write_null(NULL));
    yy_assert(!yyjson_sw_write_raw(NULL, "null"));
    yy_assert(!yyjson_sw_write_rawn(NULL, "null", 4));

    yy_assert(yyjson_sw_init_mem(&sw, sw_buf, sizeof(sw_buf), 0));
    yy_assert(yyjson_sw_obj_begin(&sw));
    yy_assert(!yyjson_sw_write_key(&sw, NULL));
    yy_assert(!yyjson_sw_ok(&sw));
    yy_assert(yyjson_sw_init_mem(&sw, sw_buf, sizeof(sw_buf), 0));
    yy_assert(!yyjson_sw_write_str(&sw, NULL));
    yy_assert(!yyjson_sw_ok(&sw));
    yy_assert(yyjson_sw_init_mem(&sw, sw_buf, sizeof(sw_buf), 0));
    yy_assert(!yyjson_sw_write_strn(&sw, "x", SIZE_MAX));
    yy_assert(!yyjson_sw_ok(&sw));
    yy_assert(yyjson_sw_init_mem(&sw, sw_buf, sizeof(sw_buf), 0));
    sv.ptr = NULL;
    sv.len = 0;
    yy_assert(!yyjson_sw_write_sv(&sw, sv));
    yy_assert(!yyjson_sw_ok(&sw));
    yy_assert(yyjson_sw_init_mem(&sw, sw_buf, sizeof(sw_buf), 0));
    yy_assert(!yyjson_sw_write_num(&sw, NULL));
    yy_assert(!yyjson_sw_ok(&sw));
    yy_assert(yyjson_sw_init_mem(&sw, sw_buf, sizeof(sw_buf), 0));
    yy_assert(!yyjson_sw_write_scalar(&sw, NULL));
    yy_assert(!yyjson_sw_ok(&sw));
    yy_assert(yyjson_sw_init_mem(&sw, sw_buf, sizeof(sw_buf), 0));
    yy_assert(!yyjson_sw_write_raw(&sw, NULL));
    yy_assert(!yyjson_sw_ok(&sw));
    yy_assert(yyjson_sw_init_mem(&sw, sw_buf, sizeof(sw_buf), 0));
    yy_assert(yyjson_sw_obj_begin(&sw));
    yy_assert(!yyjson_sw_write_keyn(&sw, "x", SIZE_MAX));
    yy_assert(!yyjson_sw_ok(&sw));
}

typedef struct write_sink {
    char *dat;
    size_t len;
    size_t cap;
    size_t calls;
} write_sink;

static bool write_sink_fn(void *ctx, const void *src, size_t len) {
    write_sink *sink = (write_sink *)ctx;
    size_t cap;
    char *dat;
    if (sink->len + len > sink->cap) {
        cap = sink->cap ? sink->cap * 2 : 128;
        while (cap < sink->len + len) cap *= 2;
        dat = (char *)realloc(sink->dat, cap);
        if (!dat) return false;
        sink->dat = dat;
        sink->cap = cap;
    }
    memcpy(sink->dat + sink->len, src, len);
    sink->len += len;
    sink->calls++;
    return true;
}

static bool write_fail_fn(void *ctx, const void *src, size_t len) {
    (void)ctx;
    (void)src;
    (void)len;
    return false;
}

/* Compare the output, then check the DOM reader accepts it back. */
static void assert_json_eq_flg(const char *got, size_t got_len,
                               const char *expect, yyjson_read_flag flg) {
    size_t expect_len = strlen(expect);

    yy_assert(got_len == expect_len);
    yy_assert(memcmp(got, expect, got_len) == 0);
#if !YYJSON_DISABLE_READER
    {
        yyjson_doc *doc = yyjson_read(got, got_len, flg);
        yy_assert(doc != NULL);
        yyjson_doc_free(doc);
    }
#else
    (void)flg;
#endif
}

static void assert_json_eq(const char *got, size_t got_len,
                           const char *expect) {
    assert_json_eq_flg(got, got_len, expect, 0);
}

static void test_writer_memory(void) {
    char buf[1024];
    yyjson_sw sw;
    yyjson_sv name = yyjson_sv_make("name", 4);
    yyjson_sv mash = yyjson_sv_make("Mash", 4);
    yy_assert(yyjson_sw_init_mem(&sw, buf, sizeof(buf), 0));
    yy_assert(yyjson_sw_obj_begin(&sw));
    yy_assert(yyjson_sw_write_key_sv(&sw, name));
    yy_assert(yyjson_sw_write_sv(&sw, mash));
    yy_assert(yyjson_sw_write_key(&sw, "values"));
    yy_assert(yyjson_sw_arr_begin(&sw));
    yy_assert(yyjson_sw_write_uint(&sw, UINT64_MAX));
    yy_assert(yyjson_sw_write_sint(&sw, INT64_MIN));
    yy_assert(yyjson_sw_write_real(&sw, 3.25));
    yy_assert(yyjson_sw_write_bool(&sw, true));
    yy_assert(yyjson_sw_write_null(&sw));
    yy_assert(yyjson_sw_write_raw(&sw, "123"));
    yy_assert(yyjson_sw_arr_end(&sw));
    yy_assert(yyjson_sw_write_key(&sw, "escaped"));
    yy_assert(yyjson_sw_write_str(&sw, "a\n\"b\\c"));
    yy_assert(yyjson_sw_obj_end(&sw));
    yy_assert(yyjson_sw_finish(&sw));
    yy_assert(yyjson_sw_ok(&sw));
    assert_json_eq(buf, yyjson_sw_len(&sw), 
                   "{\"name\":\"Mash\",\"values\":[18446744073709551615,"
                   "-9223372036854775808,3.25,true,null,123],"
                   "\"escaped\":\"a\\n\\\"b\\\\c\"}");
}

static void test_writer_multiple_documents(void) {
    char buf[64];
    yyjson_sw sw;

    yy_assert(!yyjson_sw_doc_next(NULL));

    yy_assert(yyjson_sw_init_mem(&sw, buf, sizeof(buf), 0));
    yy_assert(yyjson_sw_write_uint(&sw, 1));
    yy_assert(yyjson_sw_doc_next(&sw));
    yy_assert(yyjson_sw_write_uint(&sw, 2));
    yy_assert(yyjson_sw_finish(&sw));
    yy_assert(yyjson_sw_len(&sw) == 3);
    yy_assert(memcmp(buf, "1\n2", 3) == 0);

    yy_assert(yyjson_sw_init_mem(&sw, buf, sizeof(buf), YYJSON_WRITE_NEWLINE_AT_END));
    yy_assert(yyjson_sw_write_null(&sw));
    yy_assert(yyjson_sw_doc_next(&sw));
    yy_assert(yyjson_sw_write_bool(&sw, true));
    yy_assert(yyjson_sw_finish(&sw));
    yy_assert(yyjson_sw_len(&sw) == 10);
    yy_assert(memcmp(buf, "null\ntrue\n", 10) == 0);

    yy_assert(yyjson_sw_init_mem(&sw, buf, sizeof(buf), 0));
    yy_assert(!yyjson_sw_doc_next(&sw));
    yy_assert(!yyjson_sw_ok(&sw));
}

static void test_writer_shared_buf(void) {
    char buf[10];
    char escaped_buf[20];
    yyjson_sw sw;

    /* at max depth, output and stack share the buf;
       closing returns stack bytes to output */
    yy_assert(yyjson_sw_init_mem(&sw, buf, sizeof(buf), 0));
    yy_assert(yyjson_sw_arr_begin(&sw));
    yy_assert(yyjson_sw_arr_begin(&sw));
    yy_assert(yyjson_sw_arr_begin(&sw));
    yy_assert(yyjson_sw_write_null(&sw));
    yy_assert(yyjson_sw_arr_end(&sw));
    yy_assert(yyjson_sw_arr_end(&sw));
    yy_assert(yyjson_sw_arr_end(&sw));
    yy_assert(yyjson_sw_finish(&sw));
    assert_json_eq(buf, yyjson_sw_len(&sw), "[[[null]]]");

    /* remaining memory fits the encoded string,
       but not its 6x worst-case bound */
    yy_assert(yyjson_sw_init_mem(&sw, escaped_buf, sizeof(escaped_buf), 0));
    yy_assert(yyjson_sw_arr_begin(&sw));
    yy_assert(yyjson_sw_write_str(&sw, "1234567890"));
    yy_assert(yyjson_sw_write_str(&sw, "a\n"));
    yy_assert(yyjson_sw_arr_end(&sw));
    yy_assert(yyjson_sw_finish(&sw));
    assert_json_eq(escaped_buf, yyjson_sw_len(&sw),
                 "[\"1234567890\",\"a\\n\"]");
}

static void test_writer_generic_scalar(void) {
#if !YYJSON_DISABLE_READER
    static const char json[] = "[null,true,42,-3,1.5,\"text\"]";
    char buf[sizeof(json) - 1];
    yyjson_doc *doc = yyjson_read(json, sizeof(json) - 1, 0);
    yyjson_val *arr;
    yyjson_val raw = {0};
    yyjson_sw sw;


    yy_assert(doc != NULL);
    arr = yyjson_doc_get_root(doc);
    yy_assert(yyjson_sw_init_mem(&sw, buf, sizeof(buf), 0));
    yy_assert(yyjson_sw_arr_begin(&sw));
    for (size_t i = 0; i < yyjson_arr_size(arr); i++) {
        yy_assert(yyjson_sw_write_scalar(&sw, yyjson_arr_get(arr, i)));
    }
    yy_assert(yyjson_sw_arr_end(&sw));
    yy_assert(yyjson_sw_finish(&sw));
    assert_json_eq(buf, yyjson_sw_len(&sw), json);

    yy_assert(yyjson_set_raw(&raw, "123", 3));
    yy_assert(yyjson_sw_init_mem(&sw, buf, sizeof(buf), 0));
    yy_assert(yyjson_sw_write_scalar(&sw, &raw));
    yy_assert(yyjson_sw_finish(&sw));
    assert_json_eq(buf, yyjson_sw_len(&sw), "123");

    yy_assert(yyjson_sw_init_mem(&sw, buf, sizeof(buf), 0));
    yy_assert(!yyjson_sw_write_scalar(&sw, arr));
    yy_assert(!yyjson_sw_ok(&sw));
    yyjson_doc_free(doc);
#endif
}

static void test_writer_stream(void) {
    char window[64];
    char large[401];
    char unicode[401];
    char clean[4097];
    write_sink sink = {0};
    yyjson_sw sw;

    for (size_t i = 0; i < sizeof(large) - 1; i++) {
        large[i] = (i % 31 == 0) ? '\n' : (char)('a' + i % 26);
    }
    large[sizeof(large) - 1] = '\0';
    for (size_t i = 0; i < 50; i++) {
        memcpy(unicode + i * 8, "A\xe4\xb8\xad\xf0\x9f\x98\x80", 8);
    }
    unicode[sizeof(unicode) - 1] = '\0';
    memset(clean, 'a', sizeof(clean) - 1);
    clean[sizeof(clean) - 1] = '\0';
    yy_assert(yyjson_sw_init_fn(&sw, write_sink_fn, &sink, window, sizeof(window), 0));
    yy_assert(yyjson_sw_arr_begin(&sw));
    yy_assert(yyjson_sw_write_str(&sw, large));
    yy_assert(yyjson_sw_write_str(&sw, unicode));
    yy_assert(yyjson_sw_write_str(&sw, clean));
    yy_assert(yyjson_sw_write_rawn(&sw, "{\"raw\":1}", 9));
    yy_assert(yyjson_sw_arr_end(&sw));
    yy_assert(yyjson_sw_finish(&sw));
    yy_assert(sink.calls > 1);
    yy_assert(sink.len == yyjson_sw_len(&sw));
#if !YYJSON_DISABLE_READER
    {
        yyjson_doc *doc = yyjson_read(sink.dat, sink.len, 0);
        yyjson_val *root;
        yyjson_val *val;
        yy_assert(doc != NULL);
        root = yyjson_doc_get_root(doc);
        yy_assert(yyjson_arr_size(root) == 4);
        val = yyjson_arr_get(root, 1);
        yy_assert(yyjson_get_len(val) == sizeof(unicode) - 1);
        yy_assert(memcmp(yyjson_get_str(val), unicode, sizeof(unicode) - 1) == 0);
        val = yyjson_arr_get(root, 2);
        yy_assert(yyjson_get_len(val) == sizeof(clean) - 1);
        yy_assert(memcmp(yyjson_get_str(val), clean, sizeof(clean) - 1) == 0);
        yyjson_doc_free(doc);
    }
#endif
    free(sink.dat);

    memset(&sink, 0, sizeof(sink));
    yy_assert(yyjson_sw_init_fn(&sw, write_sink_fn, &sink, window, sizeof(window), 0));
    yy_assert(yyjson_sw_obj_begin(&sw));
    yy_assert(yyjson_sw_write_keyn(&sw, clean, sizeof(clean) - 1));
    yy_assert(yyjson_sw_write_null(&sw));
    yy_assert(yyjson_sw_obj_end(&sw));
    yy_assert(yyjson_sw_finish(&sw));
#if !YYJSON_DISABLE_READER
    {
        yyjson_doc *doc = yyjson_read(sink.dat, sink.len, 0);
        yyjson_val *root;
        yy_assert(doc != NULL);
        root = yyjson_doc_get_root(doc);
        yy_assert(yyjson_obj_size(root) == 1);
        yy_assert(yyjson_obj_getn(root, clean, sizeof(clean) - 1) != NULL);
        yyjson_doc_free(doc);
    }
#endif
    free(sink.dat);
}

/* `yyjson_sw_flush()` pushes the buffered prefix without disturbing the
   document in progress: it must not change `yyjson_sw_len()`, must be a no-op
   when nothing is buffered, and the concatenated pieces must form the same
   JSON as an unflushed run. */
static void test_writer_flush(void) {
    char window[YYJSON_SW_MIN_BUF];
    write_sink sink = {0};
    yyjson_sw sw;
    size_t calls;

    yy_assert(yyjson_sw_init_fn(&sw, write_sink_fn, &sink, window, sizeof(window), 0));
    yy_assert(yyjson_sw_obj_begin(&sw));
    yy_assert(yyjson_sw_write_key(&sw, "a"));
    yy_assert(yyjson_sw_write_uint(&sw, 1));
    /* `{"a":1` still fits in the window, so nothing has been pushed yet */
    yy_assert(sink.calls == 0);
    yy_assert(yyjson_sw_len(&sw) == 6);

    yy_assert(yyjson_sw_flush(&sw));
    yy_assert(sink.calls == 1);
    yy_assert(sink.len == 6);
    yy_assert(yyjson_sw_len(&sw) == 6);

    /* flushing an empty buffer must not push a zero-length chunk */
    calls = sink.calls;
    yy_assert(yyjson_sw_flush(&sw));
    yy_assert(sink.calls == calls);
    yy_assert(yyjson_sw_len(&sw) == 6);

    /* the document continues normally across the flush boundary */
    yy_assert(yyjson_sw_write_key(&sw, "b"));
    yy_assert(yyjson_sw_arr_begin(&sw));
    yy_assert(yyjson_sw_write_str(&sw, "x"));
    yy_assert(yyjson_sw_flush(&sw));
    yy_assert(yyjson_sw_write_str(&sw, "y"));
    yy_assert(yyjson_sw_arr_end(&sw));
    yy_assert(yyjson_sw_obj_end(&sw));
    yy_assert(yyjson_sw_finish(&sw));
    yy_assert(sink.len == yyjson_sw_len(&sw));
    yy_assert(sink.len == strlen("{\"a\":1,\"b\":[\"x\",\"y\"]}"));
    yy_assert(memcmp(sink.dat, "{\"a\":1,\"b\":[\"x\",\"y\"]}", sink.len) == 0);
    free(sink.dat);

    /* memory mode has nowhere to push to, so flush succeeds as a no-op */
    {
        char buf[64];
        yy_assert(yyjson_sw_init_mem(&sw, buf, sizeof(buf), 0));
        yy_assert(yyjson_sw_arr_begin(&sw));
        yy_assert(yyjson_sw_flush(&sw));
        yy_assert(yyjson_sw_len(&sw) == 1);
        yy_assert(yyjson_sw_arr_end(&sw));
        yy_assert(yyjson_sw_finish(&sw));
        yy_assert(yyjson_sw_len(&sw) == 2);
        yy_assert(memcmp(buf, "[]", 2) == 0);
    }
}

/* The first writer error must survive every later call, and `finish()` must
   never succeed after one. */
static void test_writer_error_sticky(void) {
    char buf[64];
    yyjson_sw sw;
    yyjson_write_err err;
    const char *first;

    yy_assert(yyjson_sw_init_mem(&sw, buf, sizeof(buf), 0));
    yy_assert(yyjson_sw_obj_begin(&sw));
    yy_assert(!yyjson_sw_write_key(&sw, NULL));
    yy_assert(yyjson_sw_get_err(&sw, &err) == YYJSON_WRITE_ERROR_INVALID_PARAMETER);
    first = err.msg;

    /* calls that would otherwise be valid keep failing */
    yy_assert(!yyjson_sw_write_key(&sw, "a"));
    yy_assert(!yyjson_sw_write_uint(&sw, 1));
    yy_assert(!yyjson_sw_write_str(&sw, "x"));
    yy_assert(!yyjson_sw_arr_begin(&sw));
    yy_assert(!yyjson_sw_obj_end(&sw));
    yy_assert(!yyjson_sw_flush(&sw));
    yy_assert(!yyjson_sw_finish(&sw));
    yy_assert(!yyjson_sw_ok(&sw));

    /* and none of them replaced the original reason */
    yy_assert(yyjson_sw_get_err(&sw, &err) == YYJSON_WRITE_ERROR_INVALID_PARAMETER);
    yy_assert(err.msg == first);

    /* a writer left with an open container cannot be finished either */
    yy_assert(yyjson_sw_init_mem(&sw, buf, sizeof(buf), 0));
    yy_assert(yyjson_sw_arr_begin(&sw));
    yy_assert(!yyjson_sw_finish(&sw));
    yy_assert(!yyjson_sw_ok(&sw));
    yy_assert(!yyjson_sw_finish(&sw));
}

static void test_writer_deep_stream(void) {
#if YYJSON_WRITER_DEPTH_LIMIT && YYJSON_WRITER_DEPTH_LIMIT < 128
    enum { depth = YYJSON_WRITER_DEPTH_LIMIT };
#else
    enum { depth = 128 };
#endif
    unsigned char buf[YYJSON_SW_BUF_SIZE(depth)];
    write_sink sink = {0};
    yyjson_sw sw;
#if !YYJSON_DISABLE_READER
    yyjson_doc *doc;
#endif


    /* force output flushes while the reverse stack remains in the buf */
    yy_assert(yyjson_sw_init_fn(&sw, write_sink_fn, &sink, buf, sizeof(buf), 0));
    for (size_t i = 0; i < depth; i++) yy_assert(yyjson_sw_arr_begin(&sw));
    for (size_t i = 0; i < 1024; i++) yy_assert(yyjson_sw_write_null(&sw));
    for (size_t i = 0; i < depth; i++) yy_assert(yyjson_sw_arr_end(&sw));
    yy_assert(yyjson_sw_finish(&sw));
    yy_assert(sink.calls > 1);
#if !YYJSON_DISABLE_READER
    doc = yyjson_read(sink.dat, sink.len, 0);
    yy_assert(doc != NULL);
    yyjson_doc_free(doc);
#endif
    free(sink.dat);
}

static void test_writer_flags(void) {
    char buf[256];
    char window[64];
    write_sink sink = {0};
    yyjson_sw sw;
    yyjson_write_flag flg = YYJSON_WRITE_ESCAPE_UNICODE |
                            YYJSON_WRITE_ESCAPE_SLASHES |
                            YYJSON_WRITE_LOWERCASE_HEX |
                            YYJSON_WRITE_NEWLINE_AT_END;
    yy_assert(yyjson_sw_init_mem(&sw, buf, sizeof(buf), flg));
    yy_assert(yyjson_sw_write_str(&sw, "中/😀"));
    yy_assert(yyjson_sw_finish(&sw));
    assert_json_eq(buf, yyjson_sw_len(&sw),
                 "\"\\u4e2d\\/\\ud83d\\ude00\"\n");

    yy_assert(yyjson_sw_init_mem(&sw, buf, sizeof(buf), flg));
    yy_assert(yyjson_sw_write_str(&sw, "a/b"));
    yy_assert(yyjson_sw_finish(&sw));
    assert_json_eq(buf, yyjson_sw_len(&sw), "\"a\\/b\"\n");

    yy_assert(yyjson_sw_init_mem(&sw, buf, sizeof(buf), flg));
    yy_assert(yyjson_sw_write_bool(&sw, false));
    yy_assert(yyjson_sw_finish(&sw));
    assert_json_eq(buf, yyjson_sw_len(&sw), "false\n");

    yy_assert(yyjson_sw_init_mem(&sw, buf, sizeof(buf), YYJSON_WRITE_FP_TO_FIXED(2)));
    yy_assert(yyjson_sw_write_real(&sw, 1.234));
    yy_assert(yyjson_sw_finish(&sw));
    assert_json_eq(buf, yyjson_sw_len(&sw), "1.23");

    yy_assert(yyjson_sw_init_mem(&sw, buf, sizeof(buf), YYJSON_WRITE_PRETTY));
    yy_assert(yyjson_sw_obj_begin(&sw));
    yy_assert(yyjson_sw_write_key(&sw, "a"));
    yy_assert(yyjson_sw_arr_begin(&sw));
    yy_assert(yyjson_sw_write_uint(&sw, 1));
    yy_assert(yyjson_sw_obj_begin(&sw));
    yy_assert(yyjson_sw_write_key(&sw, "b"));
    yy_assert(yyjson_sw_write_str(&sw, "x"));
    yy_assert(yyjson_sw_obj_end(&sw));
    yy_assert(yyjson_sw_arr_end(&sw));
    yy_assert(yyjson_sw_write_key(&sw, "empty"));
    yy_assert(yyjson_sw_obj_begin(&sw));
    yy_assert(yyjson_sw_obj_end(&sw));
    yy_assert(yyjson_sw_obj_end(&sw));
    yy_assert(yyjson_sw_finish(&sw));
    assert_json_eq(buf, yyjson_sw_len(&sw),
                 "{\n"
                 "    \"a\": [\n"
                 "        1,\n"
                 "        {\n"
                 "            \"b\": \"x\"\n"
                 "        }\n"
                 "    ],\n"
                 "    \"empty\": {}\n"
                 "}");

    yy_assert(yyjson_sw_init_fn(&sw, write_sink_fn, &sink, window, sizeof(window),
                                YYJSON_WRITE_PRETTY | YYJSON_WRITE_PRETTY_TWO_SPACES));
    yy_assert(yyjson_sw_obj_begin(&sw));
    yy_assert(yyjson_sw_write_key(&sw, "a"));
    yy_assert(yyjson_sw_arr_begin(&sw));
    yy_assert(yyjson_sw_write_uint(&sw, 1));
    yy_assert(yyjson_sw_obj_begin(&sw));
    yy_assert(yyjson_sw_write_key(&sw, "b"));
    yy_assert(yyjson_sw_write_str(&sw, "x"));
    yy_assert(yyjson_sw_obj_end(&sw));
    yy_assert(yyjson_sw_arr_end(&sw));
    yy_assert(yyjson_sw_write_key(&sw, "empty"));
    yy_assert(yyjson_sw_arr_begin(&sw));
    yy_assert(yyjson_sw_arr_end(&sw));
    yy_assert(yyjson_sw_obj_end(&sw));
    yy_assert(yyjson_sw_finish(&sw));
    assert_json_eq(sink.dat, sink.len,
                 "{\n"
                 "  \"a\": [\n"
                 "    1,\n"
                 "    {\n"
                 "      \"b\": \"x\"\n"
                 "    }\n"
                 "  ],\n"
                 "  \"empty\": []\n"
                 "}");
    free(sink.dat);
}

static void test_writer_nonstandard_flags(void) {
    char buf[256];
    char window[YYJSON_SW_MIN_BUF];
    write_sink sink = {0};
    yyjson_sw sw;
    yyjson_val num;
    yyjson_write_err err;
    const char *bad = "a\xc0\xaf" "b";

    /* Infinity and NaN are never emitted, with or without the DOM flag */
    yy_assert(yyjson_sw_init_mem(&sw, buf, sizeof(buf), YYJSON_WRITE_ALLOW_INF_AND_NAN));
    yy_assert(!yyjson_sw_write_real(&sw, INFINITY));
    yy_assert(yyjson_sw_get_err(&sw, &err) == YYJSON_WRITE_ERROR_NAN_OR_INF);

    /* as-null keeps the output standard, so it is honored */
    yy_assert(yyjson_sw_init_mem(&sw, buf, sizeof(buf), YYJSON_WRITE_INF_AND_NAN_AS_NULL));
    yy_assert(yyjson_sw_arr_begin(&sw));
    yy_assert(yyjson_sw_write_real(&sw, -INFINITY));
    yy_assert(yyjson_sw_write_real(&sw, NAN));
    unsafe_yyjson_set_real(&num, INFINITY);
    yy_assert(yyjson_sw_write_num(&sw, &num));
    yy_assert(yyjson_sw_write_real(&sw, 1.5));
    yy_assert(yyjson_sw_arr_end(&sw));
    yy_assert(yyjson_sw_finish(&sw));
    assert_json_eq(buf, yyjson_sw_len(&sw), "[null,null,null,1.5]");

#if !YYJSON_DISABLE_NON_STANDARD && !YYJSON_DISABLE_UTF8_VALIDATION
    /* invalid UTF-8 fails by default, in both string and key position */
    yy_assert(yyjson_sw_init_mem(&sw, buf, sizeof(buf), 0));
    yy_assert(!yyjson_sw_write_str(&sw, bad));
    yy_assert(yyjson_sw_get_err(&sw, &err) == YYJSON_WRITE_ERROR_INVALID_STRING);
    yy_assert(yyjson_sw_init_mem(&sw, buf, sizeof(buf), 0));
    yy_assert(yyjson_sw_obj_begin(&sw));
    yy_assert(!yyjson_sw_write_key(&sw, bad));

    /* the flag passes the bytes through unchanged */
    yy_assert(yyjson_sw_init_mem(&sw, buf, sizeof(buf), YYJSON_WRITE_ALLOW_INVALID_UNICODE));
    yy_assert(yyjson_sw_obj_begin(&sw));
    yy_assert(yyjson_sw_write_key(&sw, bad));
    yy_assert(yyjson_sw_write_str(&sw, bad));
    yy_assert(yyjson_sw_obj_end(&sw));
    yy_assert(yyjson_sw_finish(&sw));
    assert_json_eq_flg(buf, yyjson_sw_len(&sw), "{\"a\xc0\xaf" "b\":\"a\xc0\xaf" "b\"}",
                       YYJSON_READ_ALLOW_INVALID_UNICODE);

    /* escaping turns them into U+FFFD, which keeps the output valid UTF-8 */
    yy_assert(yyjson_sw_init_mem(&sw, buf, sizeof(buf),
                                 YYJSON_WRITE_ALLOW_INVALID_UNICODE |
                                 YYJSON_WRITE_ESCAPE_UNICODE));
    yy_assert(yyjson_sw_write_str(&sw, bad));
    yy_assert(yyjson_sw_finish(&sw));
    assert_json_eq(buf, yyjson_sw_len(&sw), "\"a\\uFFFD\\uFFFDb\"");

    /* same result when the string is written in chunks through a sink */
    yy_assert(yyjson_sw_init_fn(&sw, write_sink_fn, &sink, window, sizeof(window),
                                YYJSON_WRITE_ALLOW_INVALID_UNICODE |
                                YYJSON_WRITE_ESCAPE_UNICODE));
    yy_assert(yyjson_sw_write_str(&sw, bad));
    yy_assert(yyjson_sw_finish(&sw));
    assert_json_eq(sink.dat, sink.len, "\"a\\uFFFD\\uFFFDb\"");
    free(sink.dat);
#else
    (void)window;
    (void)sink;
    (void)bad;
#endif
}

static void test_writer_errors(void) {
    char buf[64];
    char tiny[2];
    char depth_buf[TEST_DEPTH * 2 + 1];
    unsigned char depth_window[YYJSON_SW_MIN_BUF];
    write_sink depth_sink = {0};
    yyjson_sw sw;
    yyjson_write_err err;


    yy_assert(yyjson_sw_init_mem(&sw, buf, sizeof(buf), 0));
    yy_assert(!yyjson_sw_write_key(&sw, "bad"));
    yy_assert(yyjson_sw_get_err(&sw, &err) == YYJSON_WRITE_ERROR_INVALID_PARAMETER);
    yy_assert(err.code == YYJSON_WRITE_ERROR_INVALID_PARAMETER);

    yy_assert(yyjson_sw_init_mem(&sw, buf, sizeof(buf), 0));
    yy_assert(yyjson_sw_obj_begin(&sw));
    yy_assert(yyjson_sw_write_key(&sw, "missing"));
    yy_assert(!yyjson_sw_obj_end(&sw));

    yy_assert(yyjson_sw_init_mem(&sw, buf, sizeof(buf), 0));
    yy_assert(yyjson_sw_write_null(&sw));
    yy_assert(!yyjson_sw_write_null(&sw));

#if !YYJSON_WRITER_DEPTH_LIMIT || YYJSON_WRITER_DEPTH_LIMIT > 1024
    yy_assert(yyjson_sw_init_mem(&sw, depth_buf, sizeof(depth_buf), 0));
    for (size_t i = 0; i < TEST_DEPTH; i++) {
        yy_assert(yyjson_sw_arr_begin(&sw));
    }
    yy_assert(!yyjson_sw_arr_begin(&sw));
    yy_assert(yyjson_sw_get_err(&sw, &err) == YYJSON_WRITE_ERROR_BUFFER_LIMIT);
    yy_assert(err.code == YYJSON_WRITE_ERROR_BUFFER_LIMIT);
#endif

    yy_assert(yyjson_sw_init_mem(&sw, depth_buf, sizeof(depth_buf), 0));
    sw.max_depth = 2;
    yy_assert(yyjson_sw_arr_begin(&sw));
    yy_assert(yyjson_sw_arr_begin(&sw));
    yy_assert(!yyjson_sw_arr_begin(&sw));
    yy_assert(yyjson_sw_get_err(&sw, &err) == YYJSON_WRITE_ERROR_DEPTH);
    yy_assert(err.code == YYJSON_WRITE_ERROR_DEPTH);

#if !YYJSON_WRITER_DEPTH_LIMIT || \
    YYJSON_WRITER_DEPTH_LIMIT >= 64
    yy_assert(yyjson_sw_init_fn(&sw, write_sink_fn, &depth_sink,
                                depth_window, sizeof(depth_window), 0));
    for (size_t i = 0; i < YYJSON_SW_MIN_BUF - 1; i++) {
        yy_assert(yyjson_sw_arr_begin(&sw));
    }
    yy_assert(!yyjson_sw_arr_begin(&sw));
    yy_assert(yyjson_sw_get_err(&sw, &err) == YYJSON_WRITE_ERROR_BUFFER_LIMIT);
    yy_assert(err.code == YYJSON_WRITE_ERROR_BUFFER_LIMIT);
    free(depth_sink.dat);
#endif

    yy_assert(yyjson_sw_init_mem(&sw, buf, sizeof(buf), 0));
    yy_assert(!yyjson_sw_write_strn(&sw, "\xC0", 1));
    yy_assert(yyjson_sw_get_err(&sw, &err) == YYJSON_WRITE_ERROR_INVALID_STRING);
    yy_assert(err.code == YYJSON_WRITE_ERROR_INVALID_STRING);

    yy_assert(yyjson_sw_init_mem(&sw, buf, sizeof(buf), YYJSON_WRITE_PRETTY));
    yy_assert(yyjson_sw_write_null(&sw));
    yy_assert(yyjson_sw_finish(&sw));
    yy_assert(yyjson_sw_len(&sw) == 4);
    yy_assert(memcmp(buf, "null", 4) == 0);

    yy_assert(yyjson_sw_init_mem(&sw, buf, sizeof(buf), YYJSON_WRITE_ALLOW_INF_AND_NAN));
    yy_assert(sw.flg == YYJSON_WRITE_ALLOW_INF_AND_NAN);
    yy_assert(!yyjson_sw_write_real(&sw, HUGE_VAL));
    yy_assert(yyjson_sw_get_err(&sw, &err) == YYJSON_WRITE_ERROR_NAN_OR_INF);
    yy_assert(err.code == YYJSON_WRITE_ERROR_NAN_OR_INF);

    /* float overflow must not honor stored inf/nan flags */
    yy_assert(yyjson_sw_init_mem(&sw, buf, sizeof(buf),
                                 YYJSON_WRITE_FP_TO_FLOAT |
                                 YYJSON_WRITE_ALLOW_INF_AND_NAN));
    yy_assert(sw.flg == (YYJSON_WRITE_FP_TO_FLOAT | YYJSON_WRITE_ALLOW_INF_AND_NAN));
    yy_assert(!yyjson_sw_write_real(&sw, 1e40));
    yy_assert(!yyjson_sw_ok(&sw));

    yy_assert(yyjson_sw_init_mem(&sw, tiny, sizeof(tiny), 0));
    yy_assert(!yyjson_sw_write_null(&sw));
    yy_assert(yyjson_sw_get_err(&sw, &err) == YYJSON_WRITE_ERROR_BUFFER_LIMIT);
    yy_assert(err.code == YYJSON_WRITE_ERROR_BUFFER_LIMIT);

    yy_assert(!yyjson_sw_init_fn(&sw, write_fail_fn, NULL, NULL, YYJSON_SW_MIN_BUF, 0));
    yy_assert(yyjson_sw_get_err(&sw, &err) == YYJSON_WRITE_ERROR_INVALID_PARAMETER);
    yy_assert(err.code == YYJSON_WRITE_ERROR_INVALID_PARAMETER);

    yy_assert(yyjson_sw_init_fn(&sw, write_fail_fn, NULL, buf, sizeof(buf), 0));
    yy_assert(yyjson_sw_write_null(&sw));
    yy_assert(!yyjson_sw_finish(&sw));
    yy_assert(yyjson_sw_get_err(&sw, &err) == YYJSON_WRITE_ERROR_IO);
    yy_assert(err.code == YYJSON_WRITE_ERROR_IO);
}

#endif /* !YYJSON_DISABLE_STREAMING && !YYJSON_DISABLE_WRITER */

#if !YYJSON_DISABLE_STREAMING && !YYJSON_DISABLE_READER && \
    !YYJSON_DISABLE_WRITER && !YYJSON_FREESTANDING && \
    !YYJSON_DISABLE_FILE
static void test_file_reader_writer(void) {
    unsigned char input_buf[4096];
    unsigned char output[64];
    yyjson_sr sr;
    yyjson_sw sw;
    FILE *fp = tmpfile();

    yy_assert(fp != NULL);
    yy_assert(yyjson_sw_init_fp(&sw, fp, output, sizeof(output), 0));
    yy_assert(yyjson_sw_obj_begin(&sw));
    yy_assert(yyjson_sw_write_key(&sw, "value"));
    yy_assert(yyjson_sw_write_uint(&sw, 42));
    yy_assert(yyjson_sw_obj_end(&sw));
    yy_assert(yyjson_sw_finish(&sw));

    rewind(fp);
    yy_assert(yyjson_sr_init_fp(&sr, fp, input_buf, sizeof(input_buf), 0));
    yy_assert(yyjson_sr_obj_begin(&sr));
    yy_assert(yyjson_sr_obj_find(&sr, "value"));
    yy_assert(yyjson_sr_read_uint(&sr) == 42);
    yy_assert(yyjson_sr_obj_end(&sr));
    yy_assert(yyjson_sr_finish(&sr));
    fclose(fp);
}
#endif

/*==============================================================================
 * MARK: - Reader Test Runner
 *============================================================================*/

#if !YYJSON_DISABLE_STREAMING && !YYJSON_DISABLE_READER

static void run_all_modes(void) {


    /* run core parsing, refill, skip, error, and offset paths in every mode */
    for (size_t i = 0; i < yy_nelems(read_modes); i++) {
        g_read_mode = read_modes[i];
        test_sample();
        test_generic_scalars();
        test_number_raw_flags();
        test_strings();
        test_skip_shapes();
        test_errors();
        test_reader_pos();
        test_unconsumed_value();
    }

    /* the remaining tests cover API contracts independent of chunk size */
    g_read_mode = read_modes[1]; /* memory */
    test_raw_values();
    test_unordered_find();
    test_obj_next();
    test_invalid_utf8();
#if !YYJSON_DISABLE_NON_STANDARD
    test_invalid_utf8_key();
#endif
    test_invalid_selected_key();
    test_numbers();
    test_peek_contract();
    test_ndjson();
    test_relaxed_skip();
    test_structural_validation();
    test_deep_nesting();
    test_scratch_limit();
    test_whitespace_everywhere();

    /* exercise a large selected string through the callback path once */
    g_read_mode = read_modes[5]; /* stream, 7-byte chunks */
    test_large_string();
}

/*==============================================================================
 * MARK: - Corpus
 * Valid files are read completely. Other files are exercised for safety.
 *============================================================================*/

/* Iterative walk so a 500-deep corpus file cannot blow the C stack.
   Open containers are tracked in `ctn`; if that fills, the rest of the
   current value is skipped with the reader's own depth storage.
   Depth-limit errors return false without crashing. */
static bool test_corpus_value(yyjson_sr *sr) {
    yyjson_type ctn[TEST_DEPTH];
    size_t n = 0;
    bool need_val = true;

    while (true) {
        yyjson_sv key;
        yyjson_val val;
        yyjson_type type;

        if (need_val) {
            type = yyjson_sr_peek_type(sr);
            if (type == YYJSON_TYPE_OBJ || type == YYJSON_TYPE_ARR) {
                if (n >= TEST_DEPTH) {
                    if (!yyjson_sr_skip_value(sr)) return false;
                    if (n == 0) return yyjson_sr_ok(sr);
                    need_val = false;
                    continue;
                }
                if (type == YYJSON_TYPE_OBJ) {
                    if (!yyjson_sr_obj_begin(sr)) return false;
                } else {
                    if (!yyjson_sr_arr_begin(sr)) return false;
                }
                ctn[n++] = type;
            } else {
                val = yyjson_sr_read_scalar(sr);
                if (yyjson_get_type(&val) == YYJSON_TYPE_NONE || !yyjson_sr_ok(sr))
                    return false;
                if (n == 0) return true;
            }
            need_val = false;
        }

        if (ctn[n - 1] == YYJSON_TYPE_OBJ) {
            if (yyjson_sr_obj_next(sr, &key)) {
                need_val = true;
                continue;
            }
            if (!yyjson_sr_ok(sr) || !yyjson_sr_obj_end(sr)) return false;
        } else {
            if (yyjson_sr_arr_next(sr)) {
                need_val = true;
                continue;
            }
            if (!yyjson_sr_ok(sr) || !yyjson_sr_arr_end(sr)) return false;
        }
        n--;
        if (n == 0) return yyjson_sr_ok(sr);
    }
}

static void test_corpus_file(const char *path, bool must_pass,
                             size_t file_index) {
    static const read_input_mode pass_modes[] = {
        { READ_INPUT_INSITU, 0 },
        { READ_INPUT_MEMORY, 0 },
        { READ_INPUT_STREAM, 7 }
    };
    unsigned char buf[YYJSON_SR_DEF_BUF];
    u8 *data = NULL;
    char *input;
    size_t len = 0, mode, mode_count;
    yyjson_sr sr;
    read_feed feed;
    bool ok;

    yy_assertf(yy_file_read(path, &data, &len),
               "failed to read corpus file: %s\n", path);
    input = (char *)malloc(len + YYJSON_PADDING_SIZE);
    yy_assertf(input != NULL, "failed to copy corpus file: %s\n", path);

    mode_count = must_pass ? yy_nelems(pass_modes) : 1;
    for (mode = 0; mode < mode_count; mode++) {
        read_input_mode input_mode;
        if (must_pass) {
            input_mode = pass_modes[mode];
        } else {
            input_mode.kind = READ_INPUT_STREAM;
            input_mode.chunk = 1 + file_index % 64;
        }
        memcpy(input, data, len);
        memset(input + len, 0, YYJSON_PADDING_SIZE);
        ok = init_reader_mode(&sr, input, len, input_mode, &feed, buf, sizeof(buf), 0);
        ok = ok && test_corpus_value(&sr) && yyjson_sr_finish(&sr);
        if (must_pass) {
            yy_assertf(ok, "valid corpus file failed: %s (mode %zu)\n", path, mode);
        }
    }

    free(input);
    free(data);
}

static void test_corpus(void) {
    static const char *dirs[] = {
        "test_checker", "test_encoding", "test_parsing",
        "test_roundtrip", "test_transform", "test_yyjson"
    };
    char dir[YY_MAX_PATH], path[YY_MAX_PATH];
    size_t tested = 0, valid = 0, other = 0;

    for (size_t d = 0; d < yy_nelems(dirs); d++) {
        yy_path_combine(dir, YYJSON_TEST_DATA_PATH, "data", "json", dirs[d], NULL);
        int count;
        char **names = yy_dir_read(dir, &count);
        yy_assertf(names && count, "failed to read corpus: %s", dir);
        for (int i = 0; i < count; i++) {
            const char *name = names[i];
            bool must_pass;

            if (*name == '.') continue;
            if (d == 0) {
                must_pass = yy_str_has_prefix(name, "pass");
            } else if (d == 1) {
                must_pass = strcmp(name, "utf8.json") == 0;
            } else if (d == 2) {
                must_pass = yy_str_has_prefix(name, "y_");
            } else if (d == 3) {
                must_pass = true;
            } else if (d == 4) {
                must_pass = !yy_str_contains(name, "invalid");
            } else {
                must_pass = !strchr(name, '(');
            }
            if (must_pass) {
                valid++;
            } else {
                other++;
            }
            yy_path_combine(path, dir, name, NULL);
            test_corpus_file(path, must_pass, tested++);
        }
        yy_dir_free(names);
    }
    yy_assert(tested != 0 && valid != 0 && other != 0);
}

#endif /* !YYJSON_DISABLE_STREAMING && !YYJSON_DISABLE_READER */


/*==============================================================================
 * MARK: - Entry
 *============================================================================*/

yy_test_case(test_json_streaming) {
#if !YYJSON_DISABLE_READER
    test_memory_input_reuse();
    test_memory_insitu();
    test_reader_flags();
    test_reader_buf();
    test_reader_buf_size();
    test_reader_window_boundary();
    test_reader_null_inputs();
    run_all_modes();
    test_corpus();
#endif
#if !YYJSON_DISABLE_WRITER
    test_writer_null_inputs();
    test_writer_memory();
    test_writer_multiple_documents();
    test_writer_shared_buf();
    test_writer_generic_scalar();
    test_writer_stream();
    test_writer_flush();
    test_writer_error_sticky();
    test_writer_deep_stream();
    test_writer_flags();
    test_writer_nonstandard_flags();
    test_writer_errors();
#endif
#if !YYJSON_DISABLE_READER && \
    !YYJSON_DISABLE_WRITER && !YYJSON_FREESTANDING && \
    !YYJSON_DISABLE_FILE
    test_file_reader_writer();
#endif
}

#else
yy_test_case(test_json_streaming) {}
#endif /* #if !YYJSON_DISABLE_STREAMING */
