'@fbc:pass
type Screen
    declare property w() as integer
    x as integer
end type

property Screen.w() as integer
    return x
end property

dim s as Screen
s.x = 5
print s.w