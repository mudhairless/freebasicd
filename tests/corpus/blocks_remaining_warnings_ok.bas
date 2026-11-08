'@fbc:pass
#cmdline "-w all"
dim b1 as byte = 255
dim s1 as single = 1.5
dim x as byte
print x shl 8
print x shl 63
print 1 shl 32 + 32
if 1 then
else
if 2 then
end if
end if
if 1 then
elseif 3 then
end if
if 2 then
else
' a comment
if 3 then
end if
end if
sub expl(byval a as integer, byref b as integer)
end sub
sub arrays(a() as integer)
end sub
declare function decl(byval a as integer) as integer
function fn() as integer
fn = 300
end function
for i as byte = 0 to 126
next
for j as byte = 0 to 255
next
dim n as integer = 300
for k as byte = 0 to n
next
