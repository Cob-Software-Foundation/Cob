extern calculate_fast(x) from "math_ext.so"
extern greet(name) from "math_ext.so"
extern sum_all() from "math_ext.so"
extern kind_of(v) from "math_ext.so"

set result = calculate_fast(42)
pop(result)
pop(greet("corn"))
pop(sum_all(1, 2, 3, 4))
pop(sum_all())
pop(kind_of(7) + "/" + kind_of("seven"))
pop("n=" + calculate_fast(calculate_fast(2)))

set i = 0
while i < 3:
    pop(calculate_fast(i))
    set i = i + 1
