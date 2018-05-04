structure Thread :> THREAD =
struct

structure T = MLton.Thread
structure P = Priority
structure Bag = ListBag

exception Thread
exception IncompatiblePriorities

datatype 'a result =
         Waiting
         | Finished of 'a * int (* result, depth *)
         | Raised of exn * int

fun writeResult fr f () =
    let val r = f ()
        val d = getDepth (processorNumber ())
    in
        fr := Finished (r, d)
    end
    handle e => fr := Raised (e, getDepth (processorNumber ()))

type 'a t = 'a result ref * P.t * (P.t * Task.t) Bag.t * Q.hand * Cancellable.t

fun run (fr, bag) f () =
    ( writeResult fr f ();
      (case Bag.dump bag of
           NONE => raise Thread
         | SOME l => List.app (ignore o push) l);
      returnToSched ()
    )

fun spawn f r' =
    let
        val p = processorNumber ()
        val r = curPrio p
        val fr = ref Waiting
        val bag = Bag.new ()
        val task = newTask (Task.Thunk (run (fr, bag) f))
        val hand = (if P.pe (r, r') then push else insert) (r', task)
        val c = Task.cancellable task
    in
        (fr, r, bag, hand, c)
    end

fun poll (result, _, bag, _, _) =
    if not (Bag.isDumped bag) then NONE
    else case !result of
             Finished (x, _) => SOME x
           | Raised (e, _) => raise e
           | Waiting => raise Thread

fun sync (result, r', bag, _, c) =
    let val c = if Cancellable.isCancelled c then
                    raise Thread
                else
                    ()
        val p = processorNumber ()
        val r = curPrio p
        val _ = if P.ple (r, r') then ()
                else
                    raise IncompatiblePriorities
        fun f rt =
            if Bag.insert (bag, rt) then
                (* Successfully waiting on the thread *)
                ()
            else
        (* Bag was just dumped, so we can directly add the task *)
                ignore (push rt)
        val d = getDepth p
        val _ = if Bag.isDumped bag then ()
                else suspend f
    in
        case !result of
            Finished (x, d') => (setDepth (p, Int.max (d, d') + 1); x)
          | Raised (e, d') => (setDepth (p, Int.max (d, d') + 1); raise e)
          | Waiting => raise Thread
    end

fun fork (f, g) =
    let val r = curPrio (processorNumber ())
        val (gt as (_, _, _, h, _)) = spawn g r
        val fr = f ()
        val gr = if tryRemove (r, h) then
                     g ()
                 else
                     sync gt
    in
        (fr, gr)
    end

fun cancel (_, _, _, _, c) =
    Cancellable.cancel c
end

structure Priority = Priority
structure IO = IO

structure Basic =
struct
val init = init
val finalizePriorities = finalizePriorities
fun currentPrio () = curPrio (processorNumber ())
val numberOfProcessors = numberOfProcessors
val processorNumber = processorNumber
val suspend = suspend
val suspendIO = suspendIO
end
