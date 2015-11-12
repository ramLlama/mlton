open IO
(* val fork = MLton.Parallel.ForkJoin.fork *)

(*
val seed = MLton.Random.seed ()
val _ = case seed of
            SOME s => MLton.Random.srand s
          | NONE => (print "Randomness not seeded!\n"; ())
*)

val _ = MLton.Random.srand (Word.fromInt 943875293)

val lock = _import "Parallel_lockTake" runtime private: int ref -> unit;
val unlock = _import "Parallel_lockRelease" runtime private: int ref -> unit;

val mut = ref ~1

fun atomically f =
    (lock mut;
     f ()
     before unlock mut)

val start = Time.now ()

val stdin = ref (TextIO.getInstream TextIO.stdIn)

fun inputLine () =
    let fun iL_int line =
            case IO.input1 (!stdin) of
                NONE => NONE
              | SOME (c, is') =>
                (stdin := is';
                 if c = #"\n" then
                     SOME line
                 else
                     iL_int (line ^ (str c)))
    in
        iL_int ""
    end

fun forkn n f =
    let fun fork_int n i =
            if n = 0 then [] else
            if n = 1 then [f i] else
            let val left = Int.div (n, 2)
                val right = n - left
                val (l, r) = MLton.Parallel.ForkJoin.fork
                    ((fn () => fork_int left i),
                     (fn () => fork_int right (i + left)))
            in
                l @ r
            end
    in
        fork_int n 0
    end

fun word_to_string w =
    if Word.< (w, Word.fromInt 256) then
        String.str (Char.chr (Word.toInt w))
    else
        let val d = Word.div (w, Word.fromInt 256)
            val r = Word.mod (w, Word.fromInt 256)
        in
            (word_to_string d) ^
            (String.str (Char.chr (Word.toInt r)))
        end

fun string_to_word s =
    let val bytes = Int.div (Word.wordSize, 8)
        val ss = String.substring (SHA1.bintohex s, 0, 4)
    in
    case Word.fromString ss of
        SOME i => i
      | NONE => (print "error\n"; Word.fromInt 0)
    end

fun bad_hash s =
    let val w = string_to_word s in
        word_to_string (Word.xorb (w, Word.>> (w, 0w2)))
    end

fun new_sha sha i =
    (* atomically (fn () => *)
    bad_hash (sha ^ (Int.toString i))
    (* sha ^ (Int.toString i) *)

fun copy s =
    CharVector.tabulate (CharVector.length s,
                        (fn i => CharVector.sub (s, i)))

fun explore d l sha =
    if d <= 1 then 0 else
    1 +
    (let val r = (* atomically (fn () => MLton.Random.rand ()) *)
             string_to_word sha
        val cs = (Word.toInt (Word.mod (r, Word.fromInt 3))) + 7
        (* val _ = if d = 1 andalso l then print ((Int.toString cs) ^ "\n") else () *)
    in
        List.foldl op+ 0 (forkn cs
                                (fn i => explore (d - 1) (l andalso i = cs - 1)
                                                 (new_sha (copy sha) i)))
    end)

fun top_explore () =
    let val nodes = explore 10 true (SHA1.hash "a different seed")
        val finish = Time.now ()
        val diff = Time.-(finish, start)
        val diffi = LargeInt.toInt (Time.toMilliseconds diff)
    in
        print ("nodes: " ^ (Int.toString nodes) ^ "\n");
        print ("exectime " ^ (Int.toString diffi) ^ "\n");
        OS.Process.exit OS.Process.success
    end

fun inploop () =
    case inputLine () of
        NONE => OS.Process.exit OS.Process.success
      | SOME l =>
        (print ("Hi, " ^ l ^ "\n");
         inploop ())

val _ = MLton.Parallel.ForkJoin.forkLat true (top_explore, inploop)
(* val _ = loop fibs fibs *)