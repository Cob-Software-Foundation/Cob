_MakeCache = False
# many calls and deep (but legal) recursion
recipe fib(n):
    if n < 2:
        serve n
    serve fib(n - 1) + fib(n - 2)

recipe depth(n):
    if n == 0:
        serve 0
    serve 1 + depth(n - 1)

recipe bump(x):
    serve x + 1

set total = 0
set i = 0
while i < 100000:
    set total = bump(total)
    set i = i + 1
pop("recipe stress: fib(22)=" + fib(22) + " depth=" + depth(400) + " calls=" + total)
