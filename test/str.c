#include "util/str.h"
#include "util/prelude.h"
#include <stdlib.h>
#include <string.h>

int
main() {
    strbuf buf = strbuf_new();

    strbuf_append(&buf, '1');
    strbuf_append(&buf, "2");
    strbuf_append(&buf, buf);
    strbuf_append(&buf, str_lit("3"));
    ww_assert(str_eq(strbuf_view(buf), str_lit("12123")));

    strbuf_clear(&buf);
    ww_assert(str_eq(strbuf_view(buf), str_lit("")));

    strbuf_append_char(&buf, '1');
    strbuf_append_cstr(&buf, "2");
    strbuf_append_buf(&buf, buf);
    strbuf_append_str(&buf, str_lit("3"));
    ww_assert(str_eq(strbuf_view(buf), str_lit("12123")));

    strbuf_clear(&buf);
    ww_assert(str_eq(strbuf_view(buf), str_lit("")));

    strbuf_free(&buf);

    buf = str_clone(str_lit("123456"));
    strbuf_append(&buf, "7890");
    ww_assert(str_eq(strbuf_view(buf), str_lit("1234567890")));

    strbuf_free(&buf);

    ww_assert(str_eq(str_lit("1"), str_lit("1")));
    ww_assert(str_eq(str_lit(""), str_lit("")));
    ww_assert(!str_eq(str_lit("1"), str_lit("2")));
    ww_assert(!str_eq(str_lit("1"), str_lit("")));

    ww_assert(str_eq(str_lit("123456"), str_slice(str_lit("123456"), 0, 6)));
    ww_assert(str_eq(str_lit("12"), str_slice(str_lit("123456"), 0, 2)));
    ww_assert(str_eq(str_lit("34"), str_slice(str_lit("123456"), 2, 4)));
    ww_assert(str_eq(str_lit("56"), str_slice(str_lit("123456"), 4, 6)));

    ww_assert(str_index(str_lit("123456"), '3', 0) == 2);
    ww_assert(str_index(str_lit("123456"), '0', 0) == -1);
    ww_assert(str_index(str_lit("12341234"), '1', 0) == 0);
    ww_assert(str_index(str_lit("12341234"), '1', 1) == 4);
    ww_assert(str_index(str_lit("12341234"), '4', 4) == 7);

    struct strs strs = str_split(str_lit("1:"), ':');
    ww_assert(strs.len == 2);
    ww_assert(str_eq(strs.data[0], str_lit("1")));
    ww_assert(str_eq(strs.data[1], str_lit("")));
    strs_free(strs);

    strs = str_split(str_lit(":1"), ':');
    ww_assert(strs.len == 2);
    ww_assert(str_eq(strs.data[0], str_lit("")));
    ww_assert(str_eq(strs.data[1], str_lit("1")));
    strs_free(strs);

    strs = str_split(str_lit("1:1"), ':');
    ww_assert(strs.len == 2);
    ww_assert(str_eq(strs.data[0], strs.data[1]));
    ww_assert(str_eq(strs.data[0], str_lit("1")));
    strs_free(strs);

    strs = str_split(str_lit(":"), ':');
    ww_assert(strs.len == 2);
    ww_assert(str_eq(strs.data[0], strs.data[1]));
    ww_assert(str_eq(strs.data[0], str_lit("")));
    strs_free(strs);

    strs = str_split(str_lit("::1:1:1::"), ':');
    ww_assert(strs.len == 7);
    ww_assert(str_eq(strs.data[0], strs.data[1]));
    ww_assert(str_eq(strs.data[1], strs.data[5]));
    ww_assert(str_eq(strs.data[5], strs.data[6]));
    ww_assert(str_eq(strs.data[0], str_lit("")));

    ww_assert(str_eq(strs.data[2], strs.data[3]));
    ww_assert(str_eq(strs.data[3], strs.data[4]));
    ww_assert(str_eq(strs.data[2], str_lit("1")));

    ww_assert(strs_index(strs, str_lit("1"), 0) == 2);
    ww_assert(strs_index(strs, str_lit("1"), 2) == 2);
    ww_assert(strs_index(strs, str_lit("1"), 3) == 3);
    ww_assert(strs_index(strs, str_lit(""), 0) == 0);
    ww_assert(strs_index(strs, str_lit(""), 2) == 5);
    strs_free(strs);

    char *cstr = str_clone_cstr(str_lit("1234"));
    ww_assert(strcmp(cstr, "1234") == 0);
    ww_assert(str_eq(str_from(cstr), str_lit("1234")));
    ww_assert(str_eq(str_from("4321"), str_lit("4321")));
    free(cstr);

    buf = (strbuf){};
    cstr = strbuf_clone_cstr(buf);
    ww_assert(*cstr == '\0');
    free(cstr);

    buf = strbuf_new();
    cstr = strbuf_clone_cstr(buf);
    ww_assert(*cstr == '\0');
    free(cstr);

    strbuf_append(&buf, "1234");
    cstr = strbuf_clone_cstr(buf);
    ww_assert(strcmp(cstr, "1234") == 0);
    free(cstr);

    strbuf_free(&buf);

    buf = strbuf_new();
    for (size_t i = 0; i < 32; i++) {
        strbuf_append(&buf, "12341234");
    }

    strbuf buf2 = strbuf_clone(buf);
    ww_assert(str_eq(strbuf_view(buf), strbuf_view(buf2)));

    strbuf_free(&buf2);
    strbuf_free(&buf);

    struct str_halves halves = str_halves(str_from("="), '=');
    ww_assert(str_eq(halves.a, halves.b));
    ww_assert(str_eq(halves.a, str_lit("")));

    halves = str_halves(str_from("a="), '=');
    ww_assert(str_eq(halves.a, str_lit("a")));
    ww_assert(str_eq(halves.b, str_lit("")));

    halves = str_halves(str_from("=b"), '=');
    ww_assert(str_eq(halves.a, str_lit("")));
    ww_assert(str_eq(halves.b, str_lit("b")));

    halves = str_halves(str_from("a=b"), '=');
    ww_assert(str_eq(halves.a, str_lit("a")));
    ww_assert(str_eq(halves.b, str_lit("b")));

    halves = str_halves(str_from(""), '=');
    ww_assert(str_eq(halves.a, halves.b));
    ww_assert(str_eq(halves.a, str_lit("")));
}
