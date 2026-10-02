# HDP — High Definition Programming

A programming language simpler than Python.

HDP is written in C++ (one file, `hdp.cpp`, no libraries needed).

## Build it

```
g++ -std=c++17 -O2 -o hdp hdp.cpp            # Linux / Mac / Windows (MinGW)
cl /std:c++17 /EHsc /O2 hdp.cpp               # Windows (Visual Studio)
```

## Run it

```
hdp examples/tour.hdp       # run a program
hdp                          # type code line by line (type exit to quit)
```

## The whole language on one page

```
# comments start with #

say "Hello"                       # print
say "Total:", 5 + 3               # several things, separated by spaces
name = ask "Your name? "          # read input (typing 42 gives the number 42)

x = 10                            # variables: no types, no 'let', no 'var'
x += 5                            # also -=  *=  /=

7 / 2         → 3.5               # only one kind of number
2 ^ 3         → 8                 # power
17 mod 5      → 2                 # remainder (% works too)
"Age: " + 30  → "Age: 30"         # text + number just works
"-" * 5       → "-----"

if x > 10                         # no colons, no brackets, indent however you like
    say "big"
else if x is 10                   # 'is', '=' and '==' all mean "equals" in a condition
    say "ten"
else
    say "small"
end

repeat 3 times ... end
for i from 1 to 10 ... end        # includes 10; 'step 2' optional
for i from 10 to 1 ... end        # counts down by itself
for item in my_list ... end
while x < 100 ... end
stop                              # leave the loop  (break)
skip                              # next round      (continue)

and   or   not   in               # if "a" in word ...   if x is not 5 ...
true / false  (or yes / no)   nothing

fruits = ["apple", "mango"]
fruits[0]      → first item       fruits[-1] → last item

function add(a, b)                # can be called before it's written
    return a + b
end
```

## Built-in functions

| Lists & text | Text only | Maths & other |
|---|---|---|
| `size(x)` / `length(x)` | `upper(t)`, `lower(t)` | `round(x)`, `round(x, 2)` |
| `add(list, item)` | `trim(t)` | `floor`, `ceil`, `abs`, `sqrt` |
| `insert(list, pos, item)` | `split(t)`, `split(t, ",")` | `random(6)` → 1–6, `random(5, 10)` |
| `remove(list)` (last) / `remove(list, pos)` | `join(list, ", ")` | `min(...)`, `max(...)` |
| `sort(list)`, `reverse(x)` (return copies) | `replace(t, old, new)` | `number(t)`, `text(x)`, `type(x)`, `pi` |
| `sum(list)`, `average(list)` | | |

## Why it's simpler than Python

| Python | HDP |
|---|---|
| `print("Hi")` | `say "Hi"` |
| `int(input("Age? "))` | `ask "Age? "` |
| `if x == 5:` + exact indentation | `if x is 5` … `end` |
| `for i in range(1, 11):` | `for i from 1 to 10` |
| `for _ in range(3):` | `repeat 3 times` |
| `"Age: " + str(30)` | `"Age: " + 30` |
| `7 // 2` vs `7 / 2`, `int` vs `float` | just numbers |
| `break` / `continue` | `stop` / `skip` |
| `IndentationError`, tracebacks | "Line 4: the 'if' on line 2 is missing its 'end'" |

Not included (yet): dictionaries, classes, files and imports.
