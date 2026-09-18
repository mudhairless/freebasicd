' Block-scoped Dims shadow outer names and die at the closer; every statement
' block (scope/if/for/while/do/select/with) declares one (FreeBASIC.md 88).
' Valid FreeBASIC: must compile, and our parser must emit nothing.
type pt
    x as integer
    y as integer
end type
dim p as pt
dim x as integer = 1
scope
    dim x as string = "Hello"
    #print typeof(x)
end scope
x = x + 1
if true then
    dim x as string
    #print typeof(x)
end if
dim i as integer
for i = 1 to 3
    dim x as string
    #print typeof(x)
next i
while false
    dim x as string
wend
do
    dim x as string
loop
select case 1
case 1
    dim x as string
    #print typeof(x)
end select
with p
    dim z as integer = 5
    .x = z
end with
x = x + 1