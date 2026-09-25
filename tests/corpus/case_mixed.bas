'@fbc:pass
' Keywords are case-insensitive, as fbc matches them: this all-caps block
' structure compiles clean, and every keyword here must classify as one (upper
' case was once lexed as plain identifiers, which silently turned each block
' into a stray closer plus an unterminated block).
SUB Main()
  DIM x AS INTEGER = 3
  DIM i AS INTEGER
  If x > 1 THEN
    PRINT "hi"
  END IF
  For i = 1 TO 2
    PRINT i
  NEXT
  DO
    x -= 1
  LOOP UNTIL x = 0
  WHILE x < 0
    x += 1
  WEND
  SELECT CASE x
    CASE 0
      PRINT "zero"
    CASE ELSE
      PRINT "other"
  END SELECT
End Sub

TYPE Pt
  x AS Integer
END TYPE

DIM SHARED g AS Integer = 7
