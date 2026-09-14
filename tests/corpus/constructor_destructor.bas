'@fbc:pass
type T
    x as integer
    declare constructor()
    declare destructor()
end type

constructor T()
    x = 1
end constructor

destructor T()
end destructor