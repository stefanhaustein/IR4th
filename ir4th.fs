\ ir4th.fs - Gforth compatibility prelude for IR4th v0.3

\ Allow direct self-recursion in ': ... ;' words
: : ['] : execute reveal ;

\ At compile time in word bodies, A: is a no-op prefix before an address literal
: A: ; immediate

\ Fixed-size named data buffers: <size> buffer: <name>
: buffer: ( n "name" -- ) create align allot ;

\ Override '{ ... }' to support '| <locals>' zero-initialization in Gforth 0.7.3
create ir4-hdr 256 allot
variable ir4-hlen
variable ir4-nloc
variable ir4-mode \ 0=inputs, 1=locals, 2=outputs

: ir4-append ( addr len -- )
  dup ir4-hlen @ + 250 > abort" IR4th header too long"
  ir4-hdr ir4-hlen @ + swap dup ir4-hlen +! move
  bl ir4-hdr ir4-hlen @ + c! 1 ir4-hlen +! ;

' { constant ir4-orig-{

: { ( -- )
  0 ir4-hlen !
  0 ir4-nloc !
  0 ir4-mode !
  begin
    begin parse-name dup 0= while 2drop refill 0= abort" unexpected EOF in '{ ... }'" repeat
    2dup s" }" str= if 2drop true else
      2dup s" |" str= if 2drop 1 ir4-mode ! false else
      2dup s" --" str= if 2drop 2 ir4-mode ! false else
        ir4-mode @ 2 < if
          2dup s" W:" str= >r
          2dup s" A:" str= >r
          2dup s" F:" str= r> r> or or if
            2drop
          else
            ir4-mode @ 1 = if 1 ir4-nloc +! then
            ir4-append
          then
        else
          2drop
        then
        false
      then then
    then
  until
  s" -- }" ir4-append
  ir4-nloc @ 0 ?do 0 postpone literal loop
  ir4-orig-{ execute
  ir4-hdr ir4-hlen @ evaluate
; immediate

: a+ + ;
: a- - ;
: adiff - ;
: a= = ;
: a<> <> ;
: au< u< ;
: au> u> ;
: a@ @ ;
: a! ! ;
: sext $FF and dup $80 and if -256 or then ;
: w>a ;
: a>w ;
: arshift 0 ?do 2/ loop ;

\ Bridge Gforth's separate F-stack to IR4th's unified stack
fvariable ir4-ftmp
: F: parse-name >float drop ir4-ftmp f! ir4-ftmp @ ;
: f+ >r ir4-ftmp ! ir4-ftmp f@ r> ir4-ftmp ! ir4-ftmp f@ f+ ir4-ftmp f! ir4-ftmp @ ;
: f- >r ir4-ftmp ! ir4-ftmp f@ r> ir4-ftmp ! ir4-ftmp f@ fswap f- ir4-ftmp f! ir4-ftmp @ ;
: f* >r ir4-ftmp ! ir4-ftmp f@ r> ir4-ftmp ! ir4-ftmp f@ f* ir4-ftmp f! ir4-ftmp @ ;
: f/ >r ir4-ftmp ! ir4-ftmp f@ r> ir4-ftmp ! ir4-ftmp f@ fswap f/ ir4-ftmp f! ir4-ftmp @ ;
: w>f s>f ir4-ftmp f! ir4-ftmp @ ;
: f>w ir4-ftmp ! ir4-ftmp f@ f>s ;
