# the corn-themed standard library: every function of the plain-C modules
shuck silo

pop("-- cornmath")
pop(husk_abs(0 - 42))
pop(husk_abs(42))
pop(ear_max(3, 9))
pop(ear_min(3, 9))
pop(stalk_pow(2, 10))
pop(stalk_pow(7, 0))
pop(stalk_sqrt(99))
pop(stalk_sqrt(100))
pop(silk_mod(17, 5))
pop(silk_mod(0 - 7, 3))
pop(silk_mod(7, 0 - 3))
scatter_seed(42)
set r1 = scatter_rand(1, 6)
set r2 = scatter_rand(1, 6)
scatter_seed(42)
if scatter_rand(1, 6) == r1:
    pop("seeded run repeats (1)")
if scatter_rand(1, 6) == r2:
    pop("seeded run repeats (2)")
set inrange = 1
set k = 0
while k < 200:
    set v = scatter_rand(10, 12)
    if v < 10:
        set inrange = 0
    if v > 12:
        set inrange = 0
    set k = k + 1
pop(inrange)

pop("-- kernels")
pop(kernel_len("popcorn"))
pop(kernel_upper("Corn on the Cob"))
pop(kernel_lower("Corn ON the Cob"))
pop(kernel_slice("popcorn", 3, 4))
pop(kernel_slice("popcorn", 5, 99))
pop("[" + kernel_slice("popcorn", 99, 2) + "]")
pop(kernel_find("popcorn", "corn"))
pop(kernel_find("popcorn", "kernel"))
pop(kernel_repeat("ab", 3))
pop("[" + kernel_trim("  husk  ") + "]")
pop(kernel_replace("a-b-c", "-", "+"))
pop(kernel_replace("aaa", "a", "bb"))
pop(kernel_char("abc", 1))
pop(kernel_code("A"))
pop(kernel_from_code(104) + kernel_from_code(105))
pop(kernel_to_int("  123 ") + 1)
pop(kernel_to_int("12x"))
pop(kernel_starts("popcorn", "pop"))
pop(kernel_starts("popcorn", "corn"))
pop(kernel_ends("popcorn", "corn"))

pop("-- almanac")
set t0 = almanac_ticks()
almanac_nap(30)
if almanac_ticks() - t0 >= 25:
    pop("nap took at least 25 ms")
if almanac_now() > 1700000000:
    pop("now is after 2023")
if almanac_year() >= 2025:
    pop("year is sane")
pop(kernel_len(almanac_stamp()))

pop("-- granary")
set path = "granary_test_tmp.txt"
pop(granary_write(path, "first line\n"))
pop(granary_append(path, "second line\n"))
pop(granary_exists(path))
pop(granary_read(path))
pop(granary_remove(path))
pop(granary_exists(path))
pop(granary_write("no_such_dir_xyz/file.txt", "x"))
