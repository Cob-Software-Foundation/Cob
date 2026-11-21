# lists (bushel) and arrays (furrow)
shuck bushel
shuck furrow

pop("-- bushel: build and read")
set b = bushel_new()
pop(bushel_len(b))
bushel_push(b, 5)
bushel_push(b, "two")
bushel_push(b, 9)
pop(bushel_len(b))
pop(bushel_get(b, 0))
pop(bushel_get(b, 1))
pop(bushel_get(b, 2) + 1)

pop("-- bushel: change")
bushel_set(b, 0, "five")
pop(bushel_get(b, 0))
bushel_insert(b, 1, 100)
bushel_insert(b, 4, "end")
pop(bushel_join(b, ","))
set gone = bushel_remove(b, 1)
pop(gone)
pop(bushel_join(b, ","))
pop(bushel_len(b))

pop("-- bushel: search")
pop(bushel_find(b, "two"))
pop(bushel_find(b, 9))
pop(bushel_find(b, "9"))
pop(bushel_find(b, "nope"))

pop("-- bushel: order")
bushel_sort(b)
pop(bushel_join(b, " "))
bushel_reverse(b)
pop(bushel_join(b, " "))

pop("-- bushel: copy and extend")
set c = bushel_copy(b)
bushel_push(c, "extra")
pop(bushel_len(b))
pop(bushel_len(c))
bushel_extend(b, c)
pop(bushel_len(b))
bushel_extend(b, b)
pop(bushel_len(b))
bushel_clear(b)
pop(bushel_len(b))

pop("-- bushel: split and join")
set w = bushel_split("a,b,,c", ",")
pop(bushel_len(w))
pop(bushel_join(w, "|"))
set ch = bushel_split("abc", "")
pop(bushel_len(ch))
pop(bushel_get(ch, 2))
set nums = bushel_new()
set i = 1
while i <= 5:
    bushel_push(nums, i * i)
    set i = i + 1
pop(bushel_join(nums, "+"))

pop("-- bushel: handles go to recipes")
recipe total(list):
    set sum = 0
    set j = 0
    while j < bushel_len(list):
        set sum = sum + bushel_get(list, j)
        set j = j + 1
    serve sum
pop(total(nums))

pop("-- bushel: mistakes warn and carry on")
pop(bushel_get(b, 5))
pop(bushel_set(b, 5, 1))
pop(bushel_insert(b, 3, 1))
pop(bushel_remove(b, 0))
pop(bushel_free(c))
pop(bushel_free(c))
pop(bushel_len(c))
pop(bushel_len(0))

pop("-- furrow: slots")
set f = furrow_new(5)
pop(furrow_len(f))
pop(furrow_get(f, 3))
furrow_set(f, 0, 4)
furrow_set(f, 1, 10)
furrow_set(f, 2, 7)
furrow_set(f, 3, "kernel")
pop(furrow_sum(f))
pop(furrow_find(f, "kernel"))
pop(furrow_find(f, 7))
pop(furrow_find(f, "7"))

pop("-- furrow: swap, sort, copy, fill")
furrow_swap(f, 0, 2)
pop(furrow_get(f, 0))
pop(furrow_get(f, 2))
furrow_sort(f)
pop(furrow_get(f, 0))
pop(furrow_get(f, 1))
pop(furrow_get(f, 3))
pop(furrow_get(f, 4))
set g = furrow_copy(f)
furrow_fill(g, "x")
pop(furrow_get(g, 0) + furrow_get(g, 4))
pop(furrow_get(f, 0))
pop(furrow_len(furrow_new(0)))

pop("-- furrow: mistakes warn and carry on")
pop(furrow_get(f, 5))
pop(furrow_get(f, 0 - 1))
pop(furrow_set(f, 9, 1))
pop(furrow_swap(f, 0, 9))
pop(furrow_new(0 - 1))
pop(furrow_free(g))
pop(furrow_free(g))
pop(furrow_len(g))

pop("-- sieve of primes up to 30: furrow and bushel together")
set n = 30
set s = furrow_new(n + 1)
furrow_fill(s, 1)
furrow_set(s, 0, 0)
furrow_set(s, 1, 0)
set p = 2
while p * p <= n:
    if furrow_get(s, p) == 1:
        set m = p * p
        while m <= n:
            furrow_set(s, m, 0)
            set m = m + p
    set p = p + 1
set primes = bushel_new()
set k = 0
while k <= n:
    if furrow_get(s, k) == 1:
        bushel_push(primes, k)
    set k = k + 1
pop(bushel_join(primes, " "))
pop(furrow_sum(s))
