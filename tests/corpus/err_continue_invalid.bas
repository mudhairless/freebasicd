'@fbc:fail
' A bare `Continue` and a `Continue` whose target word is not one of fbc's
' three: error 252 each, one per statement, no cascade.
dim i as integer
do
  continue
loop
dim j as integer
for j = 1 to 2
  continue foo
next
