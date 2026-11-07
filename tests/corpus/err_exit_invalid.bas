'@fbc:fail
' A bare `Exit` (no target word) and an `Exit` whose target word is not one
' of fbc's nine: error 251 each, one per statement, no cascade.
dim i as integer
for i = 1 to 2
  exit
next
sub s()
  exit foo
end sub
