'@fbc:pass
dim x as integer = 0
do
    x += 1
    if x > 3 then exit do
    continue do
    print x
loop

do while x < 10
    x += 1
loop

do
    x -= 1
loop until x = 0
print x