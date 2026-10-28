_MakeCache = False
# the same few string/number library calls, thousands of times (each call
# reuses its module's result buffer -- a leak or a stale buffer shows here)
shuck kernels
shuck cornmath
set i = 0
set total = 0
while i < 20000:
    set total = total + kernel_len(kernel_replace("a-b-c", "-", "++"))
    set total = total + ear_max(i, 3) - ear_max(i, 3)
    set i = i + 1
pop("stdlib stress: total=" + total + " upper=" + kernel_upper("corn"))
