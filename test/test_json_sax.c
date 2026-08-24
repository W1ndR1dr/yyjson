// This file is used to test a SAX adapter built on the streaming reader.

#include "yyjson.h"
#include "yy_test_utils.h"

#if !YYJSON_DISABLE_STREAMING && !YYJSON_DISABLE_READER

/*==============================================================================
 * MARK: - SAX Adapter
 *============================================================================*/

/**
 SAX event handler.

 Each callback is invoked as the corresponding JSON token is parsed and returns
 `true` to continue or `false` to stop. Stopping is not a reader error. Any
 callback may be `NULL`, in which case the event is ignored.

 The `str`/`len` passed to `str` and `key` point into the input or reader buffer
 and are only valid for the duration of the callback. They are NUL-terminated,
 but `len` is needed for embedded NUL characters. Copy data that must live
 longer.

 The `num` callback receives a temporary `yyjson_val` valid only for the
 duration of the call. Use the `yyjson_get_*` accessors on it. With
 `YYJSON_READ_NUMBER_AS_RAW` / `YYJSON_READ_BIGNUM_AS_RAW` it may be a RAW
 value; use `yyjson_get_raw()` + `yyjson_get_len()` (it is not
 null-terminated).
 */
typedef struct yyjson_sax_handler {
    /** Called at the start of an object (`{`). */
    bool (*obj_begin)(void *ctx);
    /** Called at the end of an object (`}`). `count` is the member count. */
    bool (*obj_end)(void *ctx, size_t count);
    /** Called at the start of an array (`[`). */
    bool (*arr_begin)(void *ctx);
    /** Called at the end of an array (`]`). `count` is the element count. */
    bool (*arr_end)(void *ctx, size_t count);
    /** Called for an object member key (a string). */
    bool (*key)(void *ctx, const char *str, size_t len);
    /** Called for a string value. */
    bool (*str)(void *ctx, const char *str, size_t len);
    /** Called for a number value (see note above about RAW numbers). */
    bool (*num)(void *ctx, const yyjson_val *num);
    /** Called for a boolean value. */
    bool (*bool_val)(void *ctx, bool value);
    /** Called for a `null` value. */
    bool (*null_val)(void *ctx);
} yyjson_sax_handler;

/* This example is recursive.
   Define YYJSON_READER_DEPTH_LIMIT to bound the call stack for untrusted input. */
static bool sax_read_value(yyjson_sr *sr, const yyjson_sax_handler *h, void *ctx) {
    yyjson_type type = yyjson_sr_peek_type(sr);
    switch (type) {
        case YYJSON_TYPE_OBJ: {
            yyjson_sv key;
            size_t count = 0;

            if (!yyjson_sr_obj_begin(sr)) return false;
            if (h->obj_begin && !h->obj_begin(ctx)) return false;
            while (yyjson_sr_obj_next(sr, &key)) {
                if (h->key && !h->key(ctx, key.ptr, key.len)) return false;
                if (!sax_read_value(sr, h, ctx)) return false;
                count++;
            }
            if (!yyjson_sr_ok(sr) || !yyjson_sr_obj_end(sr)) return false;
            return !h->obj_end || h->obj_end(ctx, count);
        }

        case YYJSON_TYPE_ARR: {
            size_t count = 0;

            if (!yyjson_sr_arr_begin(sr)) return false;
            if (h->arr_begin && !h->arr_begin(ctx)) return false;
            while (yyjson_sr_arr_next(sr)) {
                if (!sax_read_value(sr, h, ctx)) return false;
                count++;
            }
            if (!yyjson_sr_ok(sr) || !yyjson_sr_arr_end(sr)) return false;
            return !h->arr_end || h->arr_end(ctx, count);
        }

        case YYJSON_TYPE_STR: {
            yyjson_sv sv = yyjson_sr_read_str(sr);
            if (!yyjson_sr_ok(sr)) return false;
            return !h->str || h->str(ctx, sv.ptr, sv.len);
        }

        case YYJSON_TYPE_NUM: {
            yyjson_val v = yyjson_sr_read_num(sr);
            if (!yyjson_sr_ok(sr)) return false;
            return !h->num || h->num(ctx, &v);
        }

        case YYJSON_TYPE_BOOL: {
            bool b = yyjson_sr_read_bool(sr);
            if (!yyjson_sr_ok(sr)) return false;
            return !h->bool_val || h->bool_val(ctx, b);
        }

        case YYJSON_TYPE_NULL:
            yyjson_sr_read_null(sr);
            if (!yyjson_sr_ok(sr)) return false;
            return !h->null_val || h->null_val(ctx);

        default:
            /* let streaming reader set error */
            (void)yyjson_sr_read_scalar(sr);
            return false;
    }
}

static bool sax_read(yyjson_sr *sr, const yyjson_sax_handler *h, void *ctx) {
    if (!sr || !h) return false;
    return sax_read_value(sr, h, ctx) && yyjson_sr_finish(sr);
}



/*==============================================================================
 * MARK: - Input Modes
 *============================================================================*/

typedef enum sax_input_mode {
    SAX_INPUT_INSITU,
    SAX_INPUT_MEMORY,
    SAX_INPUT_STREAM
} sax_input_mode;

typedef struct sax_feed {
    const char *dat;
    size_t len;
    size_t pos;
    size_t step;
} sax_feed;

static size_t sax_feed_read(void *ctx, void *dst, size_t cap) {
    sax_feed *feed = (sax_feed *)ctx;
    size_t len = feed->len - feed->pos;

    if (len > feed->step) len = feed->step;
    if (len > cap) len = cap;
    memcpy(dst, feed->dat + feed->pos, len);
    feed->pos += len;
    return len;
}

static bool sax_read_mode(const char *json,
                          sax_input_mode mode,
                          const yyjson_sax_handler *h,
                          void *ctx,
                          bool *reader_ok) {
    unsigned char buf[YYJSON_SR_DEF_BUF];
    size_t len = strlen(json);
    char *input = (char *)malloc(len + YYJSON_PADDING_SIZE);
    sax_feed feed = { input, len, 0, 1 };
    yyjson_sr sr;
    bool ok;

    yy_assert(input != NULL);
    memcpy(input, json, len);
    memset(input + len, 0, YYJSON_PADDING_SIZE);

    if (mode == SAX_INPUT_INSITU) {
        ok = yyjson_sr_init_mem(&sr, input, len, buf, sizeof(buf), YYJSON_READ_INSITU);
    } else if (mode == SAX_INPUT_MEMORY) {
        ok = yyjson_sr_init_mem(&sr, input, len, buf, sizeof(buf), 0);
    } else {
        ok = yyjson_sr_init_fn(&sr, sax_feed_read, &feed, buf, sizeof(buf), 0);
    }
    if (ok) ok = sax_read(&sr, h, ctx);
    if (reader_ok) *reader_ok = yyjson_sr_ok(&sr);

    free(input);
    return ok;
}



/*==============================================================================
 * MARK: - Event Handler
 *============================================================================*/

typedef struct sax_result {
    size_t obj_begin;
    size_t obj_end;
    size_t arr_begin;
    size_t arr_end;
    size_t keys;
    size_t strings;
    size_t uints;
    size_t sints;
    size_t reals;
    size_t bools;
    size_t trues;
    size_t nulls;
    size_t obj_counts[2];
    size_t arr_count;
    uint64_t uint_sum;
    int64_t sint_sum;
    double real_sum;
} sax_result;

static bool sax_on_obj_begin(void *ctx) {
    ((sax_result *)ctx)->obj_begin++;
    return true;
}

static bool sax_on_obj_end(void *ctx, size_t count) {
    sax_result *result = (sax_result *)ctx;

    yy_assert(result->obj_end < yy_nelems(result->obj_counts));
    result->obj_counts[result->obj_end++] = count;
    return true;
}

static bool sax_on_arr_begin(void *ctx) {
    ((sax_result *)ctx)->arr_begin++;
    return true;
}

static bool sax_on_arr_end(void *ctx, size_t count) {
    sax_result *result = (sax_result *)ctx;

    result->arr_end++;
    result->arr_count = count;
    return true;
}

static bool sax_on_key(void *ctx, const char *str, size_t len) {
    static const char *keys[] = { "n", "a", "s", "r", "neg" };
    sax_result *result = (sax_result *)ctx;
    const char *expected;

    yy_assert(result->keys < yy_nelems(keys));
    expected = keys[result->keys++];
    yy_assertf(strlen(expected) == len && memcmp(str, expected, len) == 0,
               "expected key '%s', got '%.*s'", expected, (int)len, str);
    return true;
}

static bool sax_on_str(void *ctx, const char *str, size_t len) {
    sax_result *result = (sax_result *)ctx;

    yy_assert(len == 2);
    yy_assert(memcmp(str, "x\n", 2) == 0);
    result->strings++;
    return true;
}

static bool sax_on_num(void *ctx, const yyjson_val *num) {
    sax_result *result = (sax_result *)ctx;

    if (yyjson_is_uint(num)) {
        result->uints++;
        result->uint_sum += yyjson_get_uint(num);
    } else if (yyjson_is_sint(num)) {
        result->sints++;
        result->sint_sum += yyjson_get_sint(num);
    } else {
        yy_assert(yyjson_is_real(num));
        result->reals++;
        result->real_sum += yyjson_get_real(num);
    }
    return true;
}

static bool sax_on_bool(void *ctx, bool value) {
    sax_result *result = (sax_result *)ctx;

    result->bools++;
    result->trues += value ? 1 : 0;
    return true;
}

static bool sax_on_null(void *ctx) {
    ((sax_result *)ctx)->nulls++;
    return true;
}

static const yyjson_sax_handler sax_handler = {
    sax_on_obj_begin,
    sax_on_obj_end,
    sax_on_arr_begin,
    sax_on_arr_end,
    sax_on_key,
    sax_on_str,
    sax_on_num,
    sax_on_bool,
    sax_on_null
};

static void sax_check_result(const sax_result *result) {
    yy_assert(result->obj_begin == 2 && result->obj_end == 2);
    yy_assert(result->obj_counts[0] == 2 && result->obj_counts[1] == 3);
    yy_assert(result->arr_begin == 1 && result->arr_end == 1);
    yy_assert(result->arr_count == 4);
    yy_assert(result->keys == 5 && result->strings == 1);
    yy_assert(result->uints == 1 && result->uint_sum == 1);
    yy_assert(result->sints == 1 && result->sint_sum == -2);
    yy_assert(result->reals == 1 && result->real_sum == 1.5);
    yy_assert(result->bools == 2 && result->trues == 1);
    yy_assert(result->nulls == 1);
}



/*==============================================================================
 * MARK: - Tests
 *============================================================================*/

static void test_sax_valid(void) {
    static const char json[] =
        "{\"n\":1,\"a\":[true,false,null,{\"s\":\"x\\n\",\"r\":1.5}],\"neg\":-2}";
    sax_input_mode mode;

    for (mode = SAX_INPUT_INSITU; mode <= SAX_INPUT_STREAM; mode++) {
        sax_result result = {0};
        bool ok = false;

        yy_assert(sax_read_mode(json, mode, &sax_handler, &result, &ok));
        yy_assert(ok);
        sax_check_result(&result);
    }
}

static void test_sax_empty_handler(void) {
    static const yyjson_sax_handler empty_h = {0};
    bool ok = false;

    yy_assert(sax_read_mode("{\"a\":[1,true,null,\"x\"]}", SAX_INPUT_STREAM,
                            &empty_h, NULL, &ok));
    yy_assert(ok);
}

static bool sax_stop_at_key(void *ctx, const char *str, size_t len) {
    (void)ctx;
    (void)str;
    (void)len;
    return false;
}

static void test_sax_callback_stop(void) {
    yyjson_sax_handler h = { 0 };
    bool ok = false;

    h.key = sax_stop_at_key;
    yy_assert(!sax_read_mode("{\"a\":1}", SAX_INPUT_STREAM, &h, NULL, &ok));
    yy_assert(ok);
}

static void test_sax_invalid(void) {
    static const yyjson_sax_handler empty_h = {0};
    static const char *invalid[] = {
        "[[}}",
        "[1 2]",
        "[1,]",
        "{\"a\" 1}",
        "{\"a\":1 \"b\":2}",
        "{\"a\":[1,2}}",
        "[[]",
        "[]]"
    };
    size_t i;
    sax_input_mode mode;

    for (i = 0; i < yy_nelems(invalid); i++) {
        for (mode = SAX_INPUT_INSITU; mode <= SAX_INPUT_STREAM; mode++) {
            bool ok = true;
            yy_assertf(!sax_read_mode(invalid[i], mode, &empty_h, NULL, &ok),
                       "malformed SAX input passed: %s", invalid[i]);
            yy_assertf(!ok, "malformed SAX input had no error: %s", invalid[i]);
        }
    }
}



yy_test_case(test_json_sax) {
    test_sax_valid();
    test_sax_empty_handler();
    test_sax_callback_stop();
    test_sax_invalid();
}

#else
yy_test_case(test_json_sax) {}
#endif
