# typed: true

 case 0; in "a\x0":a1, "a\0":a2; end  # parser-error: duplicate hash pattern key a 
