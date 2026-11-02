' A With block over a real UDT instance, holding the nesting shape
' that leaked three block levels in drd's engine.bas: a compound For
' line inside a plain For inside the With. The version of this shape
' in compound_statements.bas uses a Scope instead, because a With
' needs a declared variable; this file supplies one, so the corpus
' also covers the With form in code fbc accepts.

type sector
	firstWall as integer
	wallCount as integer
end type

sub clearFlags( sec as sector, flags() as integer )
	with sec
		for s as integer = 0 to .wallCount - 1
			for i as integer = 0 to 1 : flags(.firstWall + i) = 0 : next
		next
	end with
end sub

dim as sector sec
sec.firstWall = 0 : sec.wallCount = 2
dim as integer flags(0 to 3)
clearFlags( sec, flags() )
print flags(0) + flags(1)
