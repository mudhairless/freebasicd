' Legal Exit / Continue targets in every legal shape: the matching loop
' target, a target found up the stack past a non-matching inner block, loop
' targets through transparent blocks (If, Scope, With), procedure exits from
' inside loops, and Exit Select inside a Select.
dim i as integer
type T
  a as integer
end type
dim w as T
for i = 1 to 9
  do
    exit for
    continue do
  loop
  if i then
    exit for
  end if
  scope
    continue for
  end scope
  with w
    exit for
  end with
next
while 1
  continue while
  exit while
wend
dim q as integer = 1
select case q
case 1
  exit select
end select
sub s()
  dim k as integer
  for k = 1 to 9
    exit sub
  next
end sub
function f() as integer
  dim k as integer
  for k = 1 to 9
    exit function
  next
  return 0
end function
