'@fbc:fail
' REDIM and ERASE on UDTs with stdcall or parameterized constructors.
type A
    i as integer
    declare constructor stdcall()
end type
constructor A stdcall()
end constructor
type B
    i as integer
    declare destructor stdcall()
end type
destructor B stdcall()
end destructor
type C
    i as integer
    declare constructor(byval x as integer = 0)
end type
constructor C(byval x as integer)
end constructor
dim a1() as A
redim a1(1)
dim a2(1) as A
erase a2
dim b1() as B
redim b1(1)
dim b2(1) as B
erase b2
dim c1() as C
redim c1(1)
dim c2(1) as C
erase c2
