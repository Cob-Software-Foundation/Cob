# bushel -- lists that grow, a bushel of whatever you pick
#   bushel_new()  bushel_free(b)  bushel_len(b)
#   bushel_push(b, v)  bushel_get(b, i)  bushel_set(b, i, v)
#   bushel_insert(b, i, v)  bushel_remove(b, i)  bushel_clear(b)
#   bushel_find(b, v)  bushel_sort(b)  bushel_reverse(b)
#   bushel_extend(b, other)  bushel_copy(b)
#   bushel_join(b, sep)  bushel_split(text, sep)
#   bushel_at(b, i)  bushel_put(b, i, v)  bushel_with(b, v)  bushel_range(n)  bushel_str(b)
#
# Python-style lists (Cob 0.1.0) are written with brackets and are built on the last five:
#   set xs = [1, "two", 3]    set first = xs[0]    set last = xs[-1]    set xs[1] = 9
#   for x in xs:               pop(bushel_str(xs))   prints [1, 9, 3]
extern bushel_new() from "bushel"
extern bushel_free(b) from "bushel"
extern bushel_len(b) from "bushel"
extern bushel_push(b, v) from "bushel"
extern bushel_get(b, i) from "bushel"
extern bushel_set(b, i, v) from "bushel"
extern bushel_insert(b, i, v) from "bushel"
extern bushel_remove(b, i) from "bushel"
extern bushel_clear(b) from "bushel"
extern bushel_find(b, v) from "bushel"
extern bushel_sort(b) from "bushel"
extern bushel_reverse(b) from "bushel"
extern bushel_extend(b, other) from "bushel"
extern bushel_copy(b) from "bushel"
extern bushel_join(b, sep) from "bushel"
extern bushel_split(text, sep) from "bushel"
extern bushel_at(b, i) from "bushel"
extern bushel_put(b, i, v) from "bushel"
extern bushel_with(b, v) from "bushel"
extern bushel_range(n) from "bushel"
extern bushel_str(b) from "bushel"
