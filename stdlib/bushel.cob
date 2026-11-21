# bushel -- lists that grow, a bushel of whatever you pick
#   bushel_new()  bushel_free(b)  bushel_len(b)
#   bushel_push(b, v)  bushel_get(b, i)  bushel_set(b, i, v)
#   bushel_insert(b, i, v)  bushel_remove(b, i)  bushel_clear(b)
#   bushel_find(b, v)  bushel_sort(b)  bushel_reverse(b)
#   bushel_extend(b, other)  bushel_copy(b)
#   bushel_join(b, sep)  bushel_split(text, sep)
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
