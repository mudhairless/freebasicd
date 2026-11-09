'@fbc:fail
' Module constructor and destructor decoration.
sub Bad(byval x as integer) constructor
end sub
sub f() constructor 100
end sub
sub g() constructor export
end sub
sub h() constructor foo
end sub
declare sub u() constructor
type t
    x as integer
    declare sub Init()
end type
sub t.Init() constructor
end sub
