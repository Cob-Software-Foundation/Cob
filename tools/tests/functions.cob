# recipes (functions): definition, calls, recursion, scoping, evaluation order
set g = 100
set label = "g is "

recipe add(a, b):
    serve a + b

recipe fact(n):
    if n <= 1:
        serve 1
    serve n * fact(n - 1)

recipe fib(n):
    if n < 2:
        serve n
    serve fib(n - 1) + fib(n - 2)

recipe is_even(n):
    if n == 0:
        serve 1
    serve is_odd(n - 1)

recipe is_odd(n):
    if n == 0:
        serve 0
    serve is_even(n - 1)

recipe show(x):
    pop("show " + x)
    serve x

recipe tag(name):
    pop("tag " + name)
    serve name

recipe both(a, b):
    serve a + "-" + b

recipe reads_global():
    serve g + 1

recipe shadows(g):
    set g = g * 2
    serve g

recipe local_by_set():
    set g = 5
    serve g

recipe noreturn():
    pop("in noreturn")

recipe bare():
    serve

recipe first_big(limit):
    set i = 0
    while i < 100:
        set i = i + 1
        if i * i > limit:
            serve i
    serve 0

recipe build(n):
    set s = "a"
    set k = 0
    while k < n:
        set s = s + "x"
        set k = k + 1
    serve s

recipe pick(n):
    if n == 1:
        serve "one"
    elif n == 2:
        serve "two"
    else:
        serve "many"

recipe count(n):
    if n == 0:
        serve 0
    serve 1 + count(n - 1)

recipe down(n):
    serve down(n + 1)

pop(add(2, 3))
pop(fact(10))
pop(fib(15))
pop(is_even(10) + is_odd(7) * 10)
pop(later(4))

# left-to-right evaluation of operands and arguments that print
pop(show("A") + show("B"))
pop(both(tag("first"), tag("second")))
pop(show(1) - show(2))
if show(3) < show(4):
    pop("ordered")

# scoping: parameters and `set` names are local, the rest reads globals
pop(reads_global())
pop(shadows(21))
pop(g)
pop(local_by_set())
pop(g)

# serve from inside loops and if/elif/else; falling off the end gives 0
pop(first_big(50))
pop(label + pick(1) + pick(2) + pick(9))
noreturn()
pop(bare())
pop(noreturn())

# string building (the compiler used to lose the old value here)
pop(build(5))
set s = "a"
set i = 0
while i < 3:
    set s = s + "x"
    set i = i + 1
pop(s)

# recursion depth: deep but legal, then runaway
pop(count(450))
pop(down(0))
pop("survived")

# a recipe is defined below its first use (recipes are hoisted)
recipe later(x):
    serve x * 100
