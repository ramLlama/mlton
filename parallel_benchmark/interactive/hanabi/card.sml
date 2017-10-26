structure Card =
struct

datatype color = Red | Yellow | Green | Blue | White

(* Color, number, index *)
type card = color * int * int
type hcard = (color option) * (int option)

fun cdeck (c: color) : card list =
    [(c, 1, 0), (c, 1, 1), (c, 1, 2),
     (c, 2, 0), (c, 2, 1),
     (c, 3, 0), (c, 3, 1),
     (c, 4, 0), (c, 4, 1),
     (c, 5, 0)]

val deck : card list =
    (cdeck Red) @ (cdeck Yellow) @ (cdeck Green) @ (cdeck Blue) @ (cdeck White)

fun cardToString ((c, n, _): card) =
    let val cs =
            case c of
                Red => "R"
              | Yellow => "Y"
              | Green => "G"
              | Blue => "B"
              | White => "W"
        val ns = Int.toString n
    in
        cs^ns
    end

fun hcardToString ((c, n): hcard) =
    let val cs =
            case c of
                SOME Red => "R"
              | SOME Yellow => "Y"
              | SOME Green => "G"
              | SOME Blue => "B"
              | SOME White => "W"
              | NONE => "?"
        val ns = case n of
                     SOME n' => Int.toString n'
                   | NONE => "?"
    in
        cs^ns
    end

fun cardsToString l =
    List.foldl (fn (c, s) => s ^ " " ^ (cardToString c))
               ""
               l
fun hcardsToString l =
    List.foldl (fn (c, s) => s ^ " " ^ (hcardToString c))
               ""
               l

fun canBe (seen : card list) ((col, num) : hcard) : card list =
    let fun cons ((col', num', idx) : card) =
            (not (List.exists (fn c => c = (col', num', idx)) seen))
            andalso
            (case (col, num) of
                 (SOME c, SOME n) => c = col' andalso n = num'
               | (SOME c, NONE) => c = col'
               | (NONE, SOME n) => n = num'
               | (NONE, NONE) => true)
        val cb = List.filter cons deck
    in
(*        print (cardsToString cb);
        print "\n"; *)
        cb
    end

end