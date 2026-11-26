# Python-style lists (Cob 0.1.0): [..] literals, xs[i] with negative positions, set xs[i] = v, for x in xs:
shuck bushel

# literals and printing
set xs = [10, 20, "thirty", 40]
pop(bushel_str(xs))
pop(bushel_str([]))
pop(bushel_str(["a", "b",]))
pop(bushel_str(["say \"hi\"", "tab\there", "back\\slash"]))
pop(bushel_len([1, 2 + 3, 4 * 5]))
pop(bushel_str([1, 2 + 3, 4 * 5]))

# indexing: from the front, from the back
pop(xs[0])
pop(xs[2])
pop(xs[-1])
pop(xs[-4])

# storing into a position
set xs[1] = 99
set xs[-1] = "last"
pop(bushel_str(xs))

# lists inside lists
set grid = [[1, 2], [3, 4], []]
set grid[1][0] = 33
pop(grid[1][0] + grid[0][1])
pop(bushel_len(grid[2]))
pop(grid[-2][-1])

# a list is a reference, like in Python
set a = [1, 2, 3]
set b = a
set b[0] = 100
pop(a[0])
set c = bushel_copy(a)
set c[0] = 5
pop(a[0])

# for ... in
for x in xs:
    pop(x)
set total = 0
for n in bushel_range(5):
    set total = total + n
pop(total)
for p in [1, 2]:
    for q in ["x", "y"]:
        pop(p + q)
for z in []:
    pop("never")
pop(x)

# inside a recipe, and through a call
recipe total_of(items):
    set s = 0
    for v in items:
        set s = s + v
    serve s
recipe second(items):
    serve items[1]
pop(total_of([1, 2, 3, 4]))
pop(total_of(bushel_range(101)))
pop(second(["a", "b", "c"]))

# the loop sees changes made while it runs, like a Python list does
set grow = [1, 2]
set seen = 0
for g in grow:
    set seen = seen + 1
    if seen < 4:
        bushel_push(grow, g)
pop(seen)

# a bad position warns (on stderr) and gives 0, never stops the program
pop(xs[10])
set xs[5] = 1
pop(bushel_len(xs))
