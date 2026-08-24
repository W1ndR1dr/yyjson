/*
 Differential fuzzer for the streaming reader and writer.

 For each input and each reader mode (callback with several chunk sizes,
 read-only memory, in-situ) it checks four properties against the DOM reader:

 1. Accept/reject parity: a full streaming walk accepts exactly what
    `yyjson_read()` accepts, except for the two bounded-memory errors
    (`DEPTH`, `BUFFER_LIMIT`) that the DOM reader cannot report.
 2. Value parity: the walk is piped into a streaming writer and the result must
    compare equal to the DOM tree. This covers the writer as well.
 3. Skip is a superset: `skip_value()` on the root succeeds whenever the full
    walk or the DOM reader accepted the input, since skipping only tracks
    string boundaries and bracket nesting.
 4. Read-only input stays byte-identical after a non-in-situ run.
*/

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <yyjson.h>

#define FUZZ_MAX_INPUT ((size_t)60 << 10)
#define FUZZ_WORKSPACE YYJSON_SR_BUF_SIZE(FUZZ_MAX_INPUT, 1024)
/* Re-encoding can grow the input (`1e20` -> `100000000000000000000.0`). */
#define FUZZ_OUTPUT (FUZZ_MAX_INPUT * 8 + 1024)

enum {
    MODE_FN_1,
    MODE_FN_7,
    MODE_FN_64,
    MODE_MEM,
    MODE_INSITU,
    MODE_COUNT
};

typedef struct fuzz_feed {
    const uint8_t *data;
    size_t len;
    size_t pos;
    size_t chunk;
} fuzz_feed;

static size_t fuzz_read(void *ctx, void *dst, size_t cap) {
    fuzz_feed *feed = (fuzz_feed *)ctx;
    size_t left = feed->len - feed->pos;
    size_t len = left < feed->chunk ? left : feed->chunk;
    if (len > cap) len = cap;
    memcpy(dst, feed->data + feed->pos, len);
    feed->pos += len;
    return len;
}

typedef struct fuzz_sink {
    uint8_t *dst;
    size_t cap;
    size_t len;
} fuzz_sink;

static bool fuzz_write(void *ctx, const void *src, size_t len) {
    fuzz_sink *sink = (fuzz_sink *)ctx;
    if (len > sink->cap - sink->len) return false;
    memcpy(sink->dst + sink->len, src, len);
    sink->len += len;
    return true;
}

static uint8_t g_workspace[FUZZ_WORKSPACE];
static uint8_t g_input[FUZZ_MAX_INPUT + YYJSON_PADDING_SIZE];
static uint8_t g_out[FUZZ_OUTPUT];
static uint8_t g_sw_buf[YYJSON_SW_BUF_SIZE(FUZZ_MAX_INPUT)];

/* Arm a reader over a private copy of the input in one of the reader modes. */
static bool sr_init_mode(yyjson_sr *sr, int mode, fuzz_feed *feed,
                         const uint8_t *data, size_t size) {
    memcpy(g_input, data, size);
    switch (mode) {
        case MODE_MEM:
            return yyjson_sr_init_mem(sr, (char *)g_input, size, g_workspace,
                                      sizeof(g_workspace), 0);
        case MODE_INSITU:
            return yyjson_sr_init_mem(sr, (char *)g_input, size, g_workspace,
                                      sizeof(g_workspace),
                                      YYJSON_READ_INSITU);
        default:
            feed->data = g_input;
            feed->len = size;
            feed->pos = 0;
            feed->chunk = mode == MODE_FN_1 ? 1 : (mode == MODE_FN_7 ? 7 : 64);
            return yyjson_sr_init_fn(sr, fuzz_read, feed, g_workspace,
                                     sizeof(g_workspace), 0);
    }
}

/* Property 4: only in-situ mode may touch the input. */
static void check_input_intact(int mode, const uint8_t *data, size_t size) {
    if (mode != MODE_INSITU && memcmp(g_input, data, size) != 0) abort();
}

/* Recursion cap for `stream_walk_value()` below, not a library limit.
   The reader/writer themselves do not recurse, but this harness does, to
   keep the differential check simple; a deeply nested but otherwise valid
   document (well within reach of the caller-provided buffer) could overflow
   the C stack here well before the library reports any error. */
#define FUZZ_MAX_WALK_DEPTH 256

/* Read one value, forwarding it to `sw` when non-NULL.
   Writer failures stay sticky in the writer, they are not reported here.
   Past `FUZZ_MAX_WALK_DEPTH`, hands off to the library's iterative
   `skip_value()` instead of recursing further and sets `*truncated`; the
   caller must then skip the byte-for-byte comparison against the DOM tree,
   since the writer never saw the skipped part. */
static bool stream_walk_value(yyjson_sr *sr, yyjson_sw *sw, int depth,
                              bool *truncated) {
    if (depth >= FUZZ_MAX_WALK_DEPTH) {
        *truncated = true;
        return yyjson_sr_skip_value(sr);
    }
    switch (yyjson_sr_peek_type(sr)) {
        case YYJSON_TYPE_OBJ: {
            yyjson_sv key;
            if (!yyjson_sr_obj_begin(sr)) return false;
            if (sw) (void)yyjson_sw_obj_begin(sw);
            while (yyjson_sr_obj_next(sr, &key)) {
                /* The key view dies at the next reader call, so emit it now. */
                if (sw) (void)yyjson_sw_write_key_sv(sw, key);
                if (!stream_walk_value(sr, sw, depth + 1, truncated)) {
                    return false;
                }
            }
            if (!yyjson_sr_ok(sr) || !yyjson_sr_obj_end(sr)) return false;
            if (sw) (void)yyjson_sw_obj_end(sw);
            return true;
        }
        case YYJSON_TYPE_ARR:
            if (!yyjson_sr_arr_begin(sr)) return false;
            if (sw) (void)yyjson_sw_arr_begin(sw);
            while (yyjson_sr_arr_next(sr)) {
                if (!stream_walk_value(sr, sw, depth + 1, truncated)) {
                    return false;
                }
            }
            if (!yyjson_sr_ok(sr) || !yyjson_sr_arr_end(sr)) return false;
            if (sw) (void)yyjson_sw_arr_end(sw);
            return true;
        case YYJSON_TYPE_STR: {
            yyjson_sv sv = yyjson_sr_read_str(sr);
            if (!yyjson_sr_ok(sr)) return false;
            if (sw) (void)yyjson_sw_write_sv(sw, sv);
            return true;
        }
        case YYJSON_TYPE_NUM: {
            yyjson_val num = yyjson_sr_read_num(sr);
            if (!yyjson_sr_ok(sr)) return false;
            if (sw) (void)yyjson_sw_write_num(sw, &num);
            return true;
        }
        case YYJSON_TYPE_BOOL: {
            bool val = yyjson_sr_read_bool(sr);
            if (!yyjson_sr_ok(sr)) return false;
            if (sw) (void)yyjson_sw_write_bool(sw, val);
            return true;
        }
        case YYJSON_TYPE_NULL:
            yyjson_sr_read_null(sr);
            if (!yyjson_sr_ok(sr)) return false;
            if (sw) (void)yyjson_sw_write_null(sw);
            return true;
        default:
            (void)yyjson_sr_read_scalar(sr);
            return false;
    }
}

/* True if the reader stopped on an error the DOM reader cannot report. */
static bool sr_bounded_memory_err(yyjson_sr *sr) {
    yyjson_read_err err;
    yyjson_read_code code = yyjson_sr_get_err(sr, &err);
    return code == YYJSON_READ_ERROR_DEPTH ||
           code == YYJSON_READ_ERROR_BUFFER_LIMIT;
}

/* The writer may only reject what standard JSON cannot express, or a document
   nested deeper than its own stack. Anything else is a bug. */
static bool sw_expected_err(yyjson_sw *sw) {
    yyjson_write_err err;
    yyjson_write_code code = yyjson_sw_get_err(sw, &err);
    return code == YYJSON_WRITE_ERROR_NAN_OR_INF ||
           code == YYJSON_WRITE_ERROR_DEPTH;
}

/* Property 3: skipping accepts at least as much as reading. */
static void test_skip(int mode, const uint8_t *data, size_t size,
                      bool should_accept) {
    fuzz_feed feed;
    yyjson_sr sr;
    bool ok = sr_init_mode(&sr, mode, &feed, data, size);
    ok = ok && yyjson_sr_skip_value(&sr) && yyjson_sr_finish(&sr);
    if (!ok && should_accept && !sr_bounded_memory_err(&sr)) abort();
    check_input_intact(mode, data, size);
}

static void test_mode(int mode, const uint8_t *data, size_t size,
                      yyjson_val *dom_root) {
    fuzz_feed feed;
    fuzz_sink sink;
    yyjson_sr sr;
    yyjson_sw sw;
    bool stream_ok, sw_ok, truncated = false, dom_ok = dom_root != NULL;

    /* Odd modes push through a callback sink to exercise the writer's flush
       path; even modes write straight to memory. */
    sink.dst = g_out;
    sink.cap = sizeof(g_out);
    sink.len = 0;
    if (mode & 1) {
        sw_ok = yyjson_sw_init_fn(&sw, fuzz_write, &sink, g_sw_buf,
                                  sizeof(g_sw_buf), 0);
    } else {
        sw_ok = yyjson_sw_init_mem(&sw, g_out, sizeof(g_out), 0);
    }

    stream_ok = sr_init_mode(&sr, mode, &feed, data, size);
    stream_ok = stream_ok && stream_walk_value(&sr, &sw, 0, &truncated) &&
                yyjson_sr_finish(&sr);
    sw_ok = sw_ok && yyjson_sw_finish(&sw);
    if (!(mode & 1)) sink.len = (size_t)yyjson_sw_len(&sw);
    check_input_intact(mode, data, size);

    /* Property 1: accept/reject parity with the DOM reader.
       Past FUZZ_MAX_WALK_DEPTH the walk falls back to skip_value(), which is
       deliberately more lenient (property 3), so it may accept input the
       DOM reader rejects; that case is not a bug. */
    if (stream_ok != dom_ok && !(dom_ok && sr_bounded_memory_err(&sr)) &&
        !(truncated && stream_ok && !dom_ok)) {
        abort();
    }

    /* Property 2: the re-encoded document equals the DOM tree.
       Skipped once too deep for this harness to walk, so the writer never
       saw that part; skip the comparison rather than flag a false bug. */
    if (stream_ok && dom_ok && !truncated) {
        if (!sw_ok) {
            if (!sw_expected_err(&sw)) abort();
        } else {
            yyjson_doc *doc = yyjson_read((char *)g_out, sink.len, 0);
            if (!doc) abort();
            if (!yyjson_equals(dom_root, yyjson_doc_get_root(doc))) abort();
            yyjson_doc_free(doc);
        }
    }

    test_skip(mode, data, size, stream_ok || dom_ok);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    yyjson_doc *doc;
    yyjson_val *root;
    int mode;

    if (!size || size > FUZZ_MAX_INPUT) return 0;

    memcpy(g_input, data, size);
    doc = yyjson_read_opts((char *)g_input, size, 0, NULL, NULL);
    root = yyjson_doc_get_root(doc);

    for (mode = 0; mode < MODE_COUNT; mode++) {
        test_mode(mode, data, size, root);
    }

    yyjson_doc_free(doc);
    return 0;
}
