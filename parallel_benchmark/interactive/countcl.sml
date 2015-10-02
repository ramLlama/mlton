open Graphics
(*val fork = MLton.Parallel.ForkJoin.fork *)

fun seqfork (r1,r2) = ((r2 ()), (r1 ()));
val fork = MLton.Parallel.ForkJoin.fork; (*seqfork*)

val _ = openwindow NONE (512, 512)

val _ = Posix.Process.sleep (Time.fromSeconds 1)

val lastfib = ref 0;
val clicks = ref 0;

fun get_results() =
    let
        val fib_data = ("fib" ^ (Int.toString ( !lastfib )) ^ " ")
        val clicks_data = ("clicks" ^ (Int.toString ( !clicks )) ^ "")
    in
        fib_data ^ clicks_data
    end

fun fib 0 = 1
  | fib 1 = 1
  | fib n =
    let
        val (a,b) =  fork
                         (
                           (fn () => (fib (n-1))),
                           (fn () => (fib (n-2)))
                         )
    in
        a + b
    end

fun print_results () = print (get_results ());

fun fibs n =
    let val nf = fib n in
        lastfib := nf;
        (* print_results(); *)
        fibs (n + 1)
    end



fun forever () =
    let in
        (*print "Calling nextevent..."; *)
        (case nextevent () of
             MLX.Button (true, _, _, _, _, x, y, _, _, _, b, _) =>
             let
             in
                 clear ();
                 clicks := (!clicks) + 1;
                 drawtext NONE 20 20 (get_results());
                 (* print_results(); *)
                 flush ();
                 forever ()
             end
           | _ => forever ())
    end handle MLX.X s => (print ("exn: " ^ s ^ "\n"); forever ())

fun mb () =
    (case nextevent () of
         MLX.Motion (_, _, _, _, x, y, _, _, _, _, _) => forever ()
       | _ => mb ()) handle _ => mb ()

val _ = fork (fn () => mb (), fn () => fibs 0)

val _ = closewindow ()
