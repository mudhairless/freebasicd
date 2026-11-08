'@fbc:pass
dim b1 as byte = 300
const c1 as byte = 256
dim s1 as single = 4e38
print 1 shl 64
print 2 shr 100
dim x as byte
print x shl 64
if 1 then
else if 2 then
end if
end if
sub mixed(a as integer, byval b as string, c as integer = 5)
end sub
declare function decl(a as integer) as integer
dim p as sub(a as integer)
function fn() as byte
fn = 300
end function
print 1
#cmdline "-w all"
for i as byte = 0 to 127
next
for j as ulongint = 0 to 18446744073709551615ull
next
