# furrow -- arrays: a fixed-length row, planted once
#   furrow_new(n)  furrow_free(f)  furrow_len(f)
#   furrow_get(f, i)  furrow_set(f, i, v)  furrow_fill(f, v)
#   furrow_swap(f, i, j)  furrow_find(f, v)  furrow_sum(f)
#   furrow_sort(f)  furrow_copy(f)
extern furrow_new(n) from "furrow"
extern furrow_free(f) from "furrow"
extern furrow_len(f) from "furrow"
extern furrow_get(f, i) from "furrow"
extern furrow_set(f, i, v) from "furrow"
extern furrow_fill(f, v) from "furrow"
extern furrow_swap(f, i, j) from "furrow"
extern furrow_find(f, v) from "furrow"
extern furrow_sum(f) from "furrow"
extern furrow_sort(f) from "furrow"
extern furrow_copy(f) from "furrow"
